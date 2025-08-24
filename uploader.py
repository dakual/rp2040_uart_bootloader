#!/usr/bin/env python3
import serial
import time
import sys
import os
import struct
import zlib

FIRMWARE_PATH = "/Users/daghan/Documents/PlatformIO/Projects/ota/.pio/build/pico/firmware.bin"
SERILA_PORT   = "/dev/cu.usbserial-110" 
SERIAL_BAUD   = 115200


def wait_for_message(ser, expected_message, timeout_seconds=10, debug_prefix="📨"):
    start_time = time.time()
    while time.time() - start_time < timeout_seconds:
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                # print(f"{debug_prefix} '{line}'")
                if expected_message in line:
                    return True
                elif "ERROR" in line:
                    return False
        time.sleep(0.1)
    
    return False


def load_firmware():
    try:
        with open(FIRMWARE_PATH, 'rb') as f:
            firmware_data = f.read()
        return firmware_data
    except Exception as e:
        print(f"❌ Firmware dosyası okunamadı: {e}")
        return None


def send_header(ser):
    firmware_data = load_firmware()

    magic = 0x50554C42  # "BLUP"
    size  = len(firmware_data)
    crc   = zlib.crc32(firmware_data) & 0xFFFFFFFF
    
    header = struct.pack('<III', magic, size, crc)
    print(f"   Magic: 0x{magic:08X}")
    print(f"   Size: {size}")
    print(f"   CRC: 0x{crc:08X}")
    
    ser.write(header)
    ser.flush()


def send_firmware(ser):
    firmware_data = load_firmware()
    size          = len(firmware_data)
    
    chunk_size = 256
    for i in range(0, size, chunk_size):
        print(f"⏳ Chunk {i//chunk_size + 1}/{(size + chunk_size - 1)//chunk_size} gönderiliyor...")

        if not wait_for_message(ser, "CHUNK-OK"):
            return False

        remaining = size - i
        write_size = min(chunk_size, remaining)
        chunk = firmware_data[i:i+write_size]
        
        ser.write(chunk)
        ser.flush()

    return True


def main():
    print("🚀 Firmware Uploader")
    print(f"📡 Port: {SERILA_PORT}")
    print(f"📡 Baud: {SERIAL_BAUD}")
    print(f"📁 Firmware: {FIRMWARE_PATH}")
    
    try:
        ser = serial.Serial(SERILA_PORT, SERIAL_BAUD, timeout=5)
        print("✅ Serial port açıldı")

        print("⏳ Bootloader bekleniyor...")
        if not wait_for_message(ser, "BOOTLOADER-READY"):
            print("❌ Bootloader hazır olmadı!")
            ser.close()
            return False
        print("✅ Bootloader alındı!")

        print("⏳ Header gönderiliyor...")
        send_header(ser)
        if not wait_for_message(ser, "HEADER-OK"):
            print("❌ Header hatası!")
            ser.close()
            return False

        print("⏳ Firmware gönderiliyor...")
        send_firmware(ser)
        if not wait_for_message(ser, "FIRMWARE-UPLOADED"):
            print("❌ Yükleme hatası!")
            ser.close()
            return False
        print("✅ Firmware gönderildi!")

        print("⏳ Firmware doğrulanıyor...")
        if not wait_for_message(ser, "VERIFY-OK"):
            print("❌ Firmware doğrulama hatası!")
            ser.close()
            return False
        print("✅ Firmware doğrulandı!")

        if not wait_for_message(ser, "FIRMWARE-SUCCESS"):
            print("❌ Yükleme hatası!")
            ser.close()
            return False
        print("✅ Firmware başarıyla yüklendi!")

        print("⏳ Uygulamaya geçiliyor...")
        if not wait_for_message(ser, "JUMPING-TO-APP"):
            print("❌ Uygulamaya geçiş hatası!")
            ser.close()
            return False
        print("✅ Uygulamaya geçildi!")

        ser.close()
        return True
        
    except Exception as e:
        print(f"❌ Hata: {e}")
        return False


if __name__ == "__main__":
    success = main()
    if success:
        print("🎉 Başarılı!")
    else:
        print("❌ Başarısız!")
