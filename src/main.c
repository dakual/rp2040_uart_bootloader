#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "RP2040.h"
#include <string.h>
#include <stdio.h>


#define UART_ID         uart0
#define UART_TX_PIN     0
#define UART_RX_PIN     1
#define UART_BAUD       115200

#define HDR_MAGIC           0x50554C42u  // "BLUP" (LE)
#define FLASH_APP_START     0x10004000u  // (XIP) - 16KB bootloader

#ifndef XIP_BASE
#define XIP_BASE            0x10000000u
#endif
#define XIP_END             0x11000000u 
#define SRAM_BASE           0x20000000u
#define SRAM_SIZE           (256u * 1024u + 8u * 1024u)
#define SRAM_END            (SRAM_BASE + SRAM_SIZE)
#define FLASH_SECTOR_SIZE   4096u    // erase
#define FLASH_PAGE_SIZE      256u    // program

#define FLASH_TARGET_OFFSET  (FLASH_APP_START - XIP_BASE)

static inline void uart_write_str(const char *s) {
    while (*s) uart_putc_raw(UART_ID, *s++);
}

static uint32_t calculate_crc32(const uint8_t *data, uint32_t length) {
    uint32_t crc = 0xFFFFFFFF;
    
    for (uint32_t i = 0; i < length; i++) {
        crc ^= (uint32_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320; 
            } else {
                crc >>= 1;
            }
        }
    }
    
    return crc ^ 0xFFFFFFFF;
}

static void __not_in_flash_func(safe_flash_erase)(uint32_t rel_offset, uint32_t length) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(rel_offset, length);
    restore_interrupts(ints);
}

static void __not_in_flash_func(safe_flash_program)(uint32_t rel_offset, const uint8_t *data, uint32_t length) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(rel_offset, data, length);
    restore_interrupts(ints);
}

static bool uart_read_exact(uint8_t *dst, uint32_t n, uint32_t timeout_ms_per_byte) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = 0;
        while (!uart_is_readable(UART_ID)) {
            sleep_ms(1);
            if (++t > timeout_ms_per_byte) return false;
        }
        dst[i] = uart_getc(UART_ID);
    }
    return true;
}

static bool receive_and_program_firmware(uint32_t total_size) {
    uint8_t page[FLASH_PAGE_SIZE];
    uint32_t remaining     = total_size;
    uint32_t written_flash = 0; 

    while (remaining > 0) {
        uint32_t this_len = remaining > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : remaining;
        memset(page, 0xFF, sizeof(page));

        uart_write_str("CHUNK-OK\n");

        if (!uart_read_exact(page, this_len, 5000)) {
            uart_write_str("CHUNK-ERROR\n");
            return false;
        }

        uint32_t rel_off = FLASH_TARGET_OFFSET + written_flash;
        safe_flash_program(rel_off, page, FLASH_PAGE_SIZE);

        const uint8_t *flash_ptr = (const uint8_t *)(FLASH_APP_START + written_flash);
        if (memcmp(page, flash_ptr, this_len) != 0) {
            uart_write_str("FLASH-VERIFY-ERROR\n");
            return false;
        }

        written_flash   += FLASH_PAGE_SIZE;
        remaining       -= this_len;
    }
    
    uart_write_str("FIRMWARE-UPLOADED\n");
    sleep_ms(10); 
    return true;
}

static bool final_crc_verify(uint32_t size, uint32_t expected_crc) {
    uart_write_str("VERIFYING\n");
    const uint8_t *flash_start = (const uint8_t *)(FLASH_APP_START);
    uint32_t calc = calculate_crc32(flash_start, size);

    if (calc == expected_crc) {
        uart_write_str("VERIFY-OK\n");
        sleep_ms(10); 
        return true;
    } else {
        uart_write_str("VERIFY-ERROR\n");
        sleep_ms(10); 
        return false;
    }
}

static void __no_inline_not_in_flash_func(jump_to_app)(void) {
    uint32_t const *vt  = (uint32_t const *)FLASH_APP_START;
    uint32_t new_sp     = vt[0];
    uint32_t reset_addr = vt[1];
    
    if (!(reset_addr >= 0x10000000 && reset_addr <= 0x11000000)) {
        reset_addr = FLASH_APP_START + 0x100;
    }

    if (!(new_sp >= SRAM_BASE && new_sp <= SRAM_END)) {
        uart_write_str("JUMP-ERROR: BAD-SP\n");
        sleep_ms(10);
        while (true) { tight_loop_contents(); }
    }
    if (!(reset_addr >= XIP_BASE && reset_addr < XIP_END)) {
        uart_write_str("JUMP-ERROR: BAD-RESET\n");
        sleep_ms(10);
        while (true) { tight_loop_contents(); }
    }

    uart_write_str("JUMPING-TO-APP\n");
    sleep_ms(10);
    uart_deinit(UART_ID);
    
    uint32_t ints = save_and_disable_interrupts();
    SCB->VTOR     = (volatile uint32_t)FLASH_APP_START;
    typedef void (*func_ptr_t)(void);
    func_ptr_t app_reset = (func_ptr_t)reset_addr;

    restore_interrupts(ints);

    __asm volatile(
        "msr msp, %0     \n"
        "bx  %1          \n"
        :
        : "r" (new_sp), "r" (app_reset)
        : "memory"
    );

    while (true) { tight_loop_contents(); }
}

int main() {
    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    uart_write_str("BOOTLOADER-READY\n");

    while (!uart_is_readable(UART_ID)) { 
        sleep_ms(1); 
    }
    (void)uart_getc(UART_ID);

    uint8_t header[12];
    if (!uart_read_exact(header, 12, 2000)) {
        jump_to_app(); 
    }

    uint32_t magic = (uint32_t)header[0] | ((uint32_t)header[1] << 8) | ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
    uint32_t size  = (uint32_t)header[4] | ((uint32_t)header[5] << 8) | ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);
    uint32_t crc   = (uint32_t)header[8] | ((uint32_t)header[9] << 8) | ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24);

    if (magic != HDR_MAGIC) {
        uart_write_str("MAGIC-ERROR\n");
        jump_to_app(); 
    }

    uart_write_str("HEADER-OK\n");
    sleep_ms(10); 

    uint32_t erase_len = (size + (FLASH_SECTOR_SIZE - 1)) & ~(FLASH_SECTOR_SIZE - 1);
    safe_flash_erase(FLASH_TARGET_OFFSET, erase_len);

    if (!receive_and_program_firmware(size)) {
        while (true) sleep_ms(1000);
    }

    bool crc_ok = final_crc_verify(size, crc);
    if (!crc_ok) {
        while (true) sleep_ms(1000);
    }

    uart_write_str("FIRMWARE-SUCCESS\n");
    sleep_ms(10);

    jump_to_app();

    while (true) { sleep_ms(1000); }
    return 0;
}
