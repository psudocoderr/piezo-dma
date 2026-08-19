#!/usr/bin/env python3
"""
High-Speed ESP32 ADC DMA Serial CSV Logger
============================================
Reads binary framed ADC samples from ESP32 over serial at 2 Mbaud
and logs them into a local CSV file at ~80,000 samples/sec.

Header format (14 bytes):
  magic: 2 bytes (0xAA, 0x55)
  count: uint16 (2 bytes)
  epochSec: uint32 (4 bytes)
  ms: uint16 (2 bytes)
  durationUs: uint32 (4 bytes)

Payload:
  count * uint16 ADC values (2 bytes each)
"""

import sys
import time
import struct
import array
import csv
import argparse
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Error: 'pyserial' module is required. Install it using: pip install pyserial", file=sys.stderr)
    sys.exit(1)

MAGIC_BYTES = b'\xaa\x55'
HEADER_SIZE = 14
HEADER_FORMAT = '<2sHIHI'  # magic (2s), count (H), epochSec (I), ms (H), durationUs (I)

def find_serial_port():
    """Auto-detect available USB serial port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if "USB" in port.device or "ACM" in port.device or "ttyUSB" in port.device:
            return port.device
    if ports:
        return ports[0].device
    return "/dev/ttyUSB0"

def main():
    parser = argparse.ArgumentParser(description="Record 80kHz binary ADC data from ESP32 serial to CSV.")
    parser.add_argument("-p", "--port", type=str, default=None, help="Serial port (e.g., /dev/ttyUSB0 or COM3)")
    parser.add_argument("-b", "--baud", type=int, default=2000000, help="Baud rate (default: 2000000)")
    parser.add_argument("-o", "--output", type=str, default="adc_data_80khz.csv", help="Output CSV filename")
    parser.add_argument("-d", "--duration", type=float, default=None, help="Capture duration in seconds (optional)")
    args = parser.parse_args()

    port_name = args.port or find_serial_port()
    baud_rate = args.baud
    output_filename = args.output

    print("======================================================")
    print("      ESP32 80kHz ADC DMA Binary -> CSV Logger        ")
    print("======================================================")
    print(f" Port        : {port_name}")
    print(f" Baud Rate   : {baud_rate}")
    print(f" Output File : {output_filename}")
    if args.duration:
        print(f" Duration    : {args.duration} seconds")
    print("======================================================")

    try:
        ser = serial.Serial(port_name, baud_rate, timeout=1.0)
    except Exception as e:
        print(f"\n[ERROR] Failed to open serial port '{port_name}': {e}", file=sys.stderr)
        print("Tip: Make sure your ESP32 is plugged in and you have permissions (e.g. sudo usermod -a -G dialout $USER)", file=sys.stderr)
        sys.exit(1)

    # Flush rx buffer
    ser.reset_input_buffer()

    total_samples = 0
    total_blocks = 0
    sync_errors = 0
    start_time = time.time()
    last_report_time = start_time
    samples_since_report = 0

    print("\nListening for binary stream... Press Ctrl+C to stop.\n")

    with open(output_filename, mode='w', newline='', buffering=65536) as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["timestamp_sec", "sample_index", "adc_value"])

        buffer = bytearray()

        try:
            while True:
                # Read chunks from serial
                bytes_to_read = max(ser.in_waiting, 4096)
                chunk = ser.read(bytes_to_read)
                if chunk:
                    buffer.extend(chunk)

                # Search for magic header
                while len(buffer) >= HEADER_SIZE:
                    magic_idx = buffer.find(MAGIC_BYTES)
                    if magic_idx == -1:
                        # Keep last 1 byte in case header was split across reads
                        buffer = buffer[-1:]
                        break

                    if magic_idx > 0:
                        sync_errors += magic_idx
                        buffer = buffer[magic_idx:]  # Align buffer to magic header

                    if len(buffer) < HEADER_SIZE:
                        break

                    # Unpack header
                    magic, count, epoch_sec, ms, duration_us = struct.unpack_from(HEADER_FORMAT, buffer, 0)
                    packet_len = HEADER_SIZE + (count * 2)

                    if len(buffer) < packet_len:
                        # Waiting for full payload
                        break

                    # Slice out packet payload
                    payload_bytes = buffer[HEADER_SIZE:packet_len]
                    buffer = buffer[packet_len:]

                    # Unpack 16-bit unsigned samples
                    adc_values = array.array('H')
                    adc_values.frombytes(payload_bytes)

                    # Calculate microsecond step per sample
                    us_per_sample = duration_us / count if count > 0 else 12.5
                    block_start_sec = epoch_sec + (ms / 1000.0)

                    # Prepare CSV rows batch
                    rows = []
                    for idx, val in enumerate(adc_values):
                        sample_timestamp = block_start_sec + (idx * us_per_sample / 1_000_000.0)
                        rows.append((f"{sample_timestamp:.6f}", total_samples + idx, val))

                    writer.writerows(rows)

                    total_samples += count
                    samples_since_report += count
                    total_blocks += 1

                # Periodic performance reporting
                now = time.time()
                elapsed_report = now - last_report_time
                if elapsed_report >= 1.0:
                    rate = samples_since_report / elapsed_report
                    file_size_mb = Path(output_filename).stat().st_size / (1024 * 1024)
                    print(f"\r[STATUS] Samples: {total_samples:,} | Rate: {rate:,.0f} samples/sec | CSV Size: {file_size_mb:.2f} MB | Sync Loss: {sync_errors} B", end="")
                    samples_since_report = 0
                    last_report_time = now

                # Duration check
                if args.duration and (now - start_time) >= args.duration:
                    print(f"\nTarget duration of {args.duration} seconds reached.")
                    break

        except KeyboardInterrupt:
            print("\nRecording stopped by user.")

        finally:
            ser.close()
            total_elapsed = time.time() - start_time
            file_size_mb = Path(output_filename).stat().st_size / (1024 * 1024) if Path(output_filename).exists() else 0.0
            avg_rate = total_samples / total_elapsed if total_elapsed > 0 else 0

            print("\n" + "=" * 54)
            print("                Capture Summary                   ")
            print("=" * 54)
            print(f" Total Time     : {total_elapsed:.2f} seconds")
            print(f" Total Samples  : {total_samples:,}")
            print(f" Average Rate   : {avg_rate:,.0f} samples/sec")
            print(f" Saved CSV File : {output_filename} ({file_size_mb:.2f} MB)")
            print(f" Sync Drops     : {sync_errors} bytes")
            print("=" * 54)

if __name__ == "__main__":
    main()
