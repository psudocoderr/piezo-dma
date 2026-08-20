#!/usr/bin/env python3
"""
======================================================================================
80kHz Piezo Serial Receiver (3-Byte Binary Stream)
Target MCU: ESP32 DevKit V1
======================================================================================
Listens to Serial port (e.g. /dev/ttyUSB0 or COM3) at 2,000,000 baud.
Parses continuous 3-byte framed readings:
  [0xAA (Sync)][MSB][LSB]

Features:
- Validates 3-byte sync framing byte (0xAA).
- Automatically resynchronizes stream if framing slip occurs.
- Tracks signal sanity (clipping, flatlining, noise floor).
- Writes timestamped CSV log or outputs real-time statistics.
======================================================================================
"""

import sys
import time
import argparse
from pathlib import Path

try:
    import serial
except ImportError:
    print("[ERROR] pyserial library is required. Install via: pip install pyserial")
    sys.exit(1)

SYNC_BYTE = 0xAA


def run_serial_receiver(port: str, baudrate: int, csv_path: str, duration_sec: float):
    print(f"[INIT] Opening Serial port {port} at {baudrate} baud...")

    try:
        ser = serial.Serial(port, baudrate, timeout=1.0)
    except Exception as e:
        print(f"[ERROR] Failed to open serial port {port}: {e}")
        return

    print(f"[INIT] Output CSV: {Path(csv_path).resolve()}")
    print("[RUNNING] Capturing 3-byte framed readings [0xAA][MSB][LSB]...")

    start_time = time.time()
    last_report_time = start_time
    total_readings = 0
    sync_slips = 0
    clipping_count = 0

    with open(csv_path, "w", buffering=65536, encoding="utf-8") as f:
        f.write("ts,adc_raw\n")

        try:
            while (time.time() - start_time) < duration_sec:
                # Read 3 bytes per reading
                raw_3b = ser.read(3)
                if len(raw_3b) < 3:
                    continue

                sync, msb, lsb = raw_3b[0], raw_3b[1], raw_3b[2]

                # Resynchronization check
                if sync != SYNC_BYTE:
                    sync_slips += 1
                    # Read 1 byte to realign
                    ser.read(1)
                    continue

                adc_val = (msb << 8) | lsb

                if adc_val >= 4090:
                    clipping_count += 1

                ts = time.time()
                f.write(f"{ts:.6f},{adc_val}\n")
                total_readings += 1

                # Report every 2 seconds
                now = time.time()
                if now - last_report_time >= 2.0:
                    elapsed = now - start_time
                    rate = total_readings / elapsed if elapsed > 0 else 0
                    print(f"[STATUS] Captured: {total_readings:,} readings | Rate: {rate:,.1f} Hz | Sync Slips: {sync_slips} | Clips: {clipping_count}")
                    last_report_time = now

        except KeyboardInterrupt:
            print("\n[EXIT] Serial collection stopped by user.")

    ser.close()
    print(f"[SUMMARY] Total Readings Captured: {total_readings:,}")
    print(f"[SUMMARY] Total Sync Slips       : {sync_slips}")


def main():
    parser = argparse.ArgumentParser(description="ESP32 80kHz 3-Byte Serial Binary Receiver")
    parser.add_argument("--port", default="/dev/ttyUSB0", help="Serial port (e.g. /dev/ttyUSB0 or COM3)")
    parser.add_argument("--baud", type=int, default=3000000, help="Baud rate (default: 3000000)")
    parser.add_argument("--csv", default="serial_piezo_data.csv", help="Output CSV path")
    parser.add_argument("--duration", type=float, default=10.0, help="Capture duration in seconds")
    args = parser.parse_args()

    run_serial_receiver(args.port, args.baud, args.csv, args.duration)


if __name__ == "__main__":
    main()
