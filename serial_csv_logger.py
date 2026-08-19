#!/usr/bin/env python3
"""
High-Speed ESP32 Binary ADC Receiver & CSV Logger (80 kHz Optimized)
Supports:
1. Headered Binary Stream (10-byte or 16-byte header with sync 0x55AA55AA)
2. Direct Continuous Binary ADC Stream (Raw 16-bit DMA words, 0x0FFF 12-bit ADC mask)

Writes strictly 2 columns ('timestamp_iso', 'raw_adc') to CSV with microsecond-accurate batch timestamps.
"""

import sys
import time
import struct
import argparse
import csv
from datetime import datetime
import serial
import numpy as np

# Protocol Sync Word
SYNC_WORD = 0x55AA55AA

# 10-byte Header Format (piezo-dma_v9): sync(I), sequence(I), payload_len(H)
HEADER_FORMAT_10 = "<IIH"
HEADER_SIZE_10 = struct.calcsize(HEADER_FORMAT_10) # 10 bytes

# 16-byte Header Format (esp32_dma_stream): sync(I), sequence(I), sample_count(H), timestamp_us(I), crc16(H)
HEADER_FORMAT_16 = "<IIHIH"
HEADER_SIZE_16 = struct.calcsize(HEADER_FORMAT_16) # 16 bytes

def crc16_ccitt(data: bytes) -> int:
    """Computes CRC-16 CCITT (0x1021, init 0xFFFF)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

def parse_args():
    parser = argparse.ArgumentParser(description="ESP32 High-Speed ADC Binary Data Receiver & 2-Column CSV Logger")
    parser.add_argument("-p", "--port", type=str, default="/dev/ttyUSB0", help="Serial port (e.g. /dev/ttyUSB0 or COM3)")
    parser.add_argument("-b", "--baud", type=int, default=2000000, help="Baud rate (default: 2000000)")
    parser.add_argument("-o", "--output", type=str, default="adc_data.csv", help="Output CSV file path")
    parser.add_argument("-r", "--sample-rate", type=float, default=80000.0, help="Expected sampling rate in Hz (default: 80000)")
    parser.add_argument("--no-crc", action="store_true", default=False, help="Disable CRC check")
    return parser.parse_args()

def main():
    args = parse_args()

    print("==================================================")
    print("      ESP32 ADC High-Speed Binary Receiver       ")
    print("==================================================")
    print(f" Port            : {args.port}")
    print(f" Baud Rate       : {args.baud} bps")
    print(f" Sampling Rate   : {args.sample_rate:.0f} Hz")
    print(f" CSV Output      : {args.output}")
    print(" CSV Columns     : timestamp_iso, raw_adc")
    print("==================================================")

    # Open serial connection with explicit DTR/RTS pulse
    try:
        ser = serial.Serial()
        ser.port = args.port
        ser.baudrate = args.baud
        ser.timeout = 0.1
        ser.dtr = False
        ser.rts = False
        ser.open()

        # Reset pulse for ESP32 auto-reset circuit
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.2)
        ser.reset_input_buffer()
        print(f"[+] Successfully opened {args.port} at {args.baud} baud.")
    except Exception as e:
        print(f"[!] Error opening serial port {args.port}: {e}")
        print("    Please check device connection or permissions (e.g., sudo chmod 666 /dev/ttyUSB0).")
        sys.exit(1)

    # Open CSV file for writing
    try:
        csv_file = open(args.output, mode="w", newline="", buffering=2 * 1024 * 1024)
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["timestamp_iso", "raw_adc"])
        print(f"[+] Output CSV file '{args.output}' initialized.")
    except Exception as e:
        print(f"[!] Error opening output file {args.output}: {e}")
        ser.close()
        sys.exit(1)

    sample_interval = 1.0 / args.sample_rate
    sync_bytes = struct.pack("<I", SYNC_WORD)

    buffer = bytearray()
    expected_sequence = None
    total_samples = 0
    total_bytes = 0
    crc_errors = 0
    dropped_packets = 0
    
    stream_mode = None  # None = Searching, 'header' = Packet Header Mode, 'direct' = Direct Raw Stream Mode
    header_type_detected = None
    search_bytes_count = 0

    start_time = time.time()
    last_report_time = start_time
    report_samples = 0

    print("\n[>] Synchronizing with ESP32 ADC stream... Press Ctrl+C to stop.\n")

    try:
        while True:
            chunk = ser.read(32768)
            if not chunk:
                continue

            buffer.extend(chunk)
            total_bytes += len(chunk)
            search_bytes_count += len(chunk)

            # Determine Mode if not locked yet
            if stream_mode is None:
                sync_pos = buffer.find(sync_bytes)
                if sync_pos != -1:
                    stream_mode = 'header'
                    if sync_pos > 0:
                        del buffer[:sync_pos]
                elif search_bytes_count >= 4096:
                    stream_mode = 'direct'
                    print("[+] Detected Protocol: Direct Continuous Raw ADC Binary Stream (No Header)")

            # Mode 1: Direct Raw ADC Stream Processing (Continuous uint16 samples)
            if stream_mode == 'direct':
                # Process in even 2-byte aligned blocks (1024 samples = 2048 bytes)
                chunk_bytes_len = (len(buffer) // 2) * 2
                if chunk_bytes_len == 0:
                    continue

                raw_bytes = bytes(buffer[:chunk_bytes_len])
                del buffer[:chunk_bytes_len]

                recv_time = time.time()
                raw_u16 = np.frombuffer(raw_bytes, dtype=np.uint16)
                raw_adc = np.bitwise_and(raw_u16, 0x0FFF)
                num_samples = len(raw_adc)

                # Batch microsecond-accurate timestamp calculation
                batch_start_time = recv_time - (num_samples * sample_interval)
                sample_indices = np.arange(num_samples)
                sample_epochs = batch_start_time + (sample_indices * sample_interval)

                rows = [
                    (
                        datetime.fromtimestamp(sample_epochs[i]).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                        int(raw_adc[i])
                    )
                    for i in range(num_samples)
                ]

                csv_writer.writerows(rows)
                total_samples += num_samples
                report_samples += num_samples

            # Mode 2: Headered Binary Packet Processing (0x55AA55AA Sync)
            elif stream_mode == 'header':
                while len(buffer) >= 10:
                    sync_pos = buffer.find(sync_bytes)
                    if sync_pos == -1:
                        buffer = buffer[-3:]
                        break
                    if sync_pos > 0:
                        del buffer[:sync_pos]
                    if len(buffer) < 10:
                        break

                    # Auto-detect header structure
                    if header_type_detected is None:
                        sync_val, seq, val1 = struct.unpack(HEADER_FORMAT_10, buffer[:10])
                        if len(buffer) >= 16:
                            _, _, count_16, ts_16, crc_16 = struct.unpack(HEADER_FORMAT_16, buffer[:16])
                            if val1 > 0 and (val1 % 2 == 0) and (count_16 == val1 // 2):
                                header_type_detected = 16
                                print("[+] Detected Protocol: 16-byte Header (esp32_dma_stream with CRC)")
                            else:
                                header_type_detected = 10
                                print("[+] Detected Protocol: 10-byte Header (piezo-dma_v9)")
                        else:
                            break

                    hdr_size = HEADER_SIZE_16 if header_type_detected == 16 else HEADER_SIZE_10
                    if len(buffer) < hdr_size:
                        break

                    if header_type_detected == 10:
                        _, sequence, payload_len = struct.unpack(HEADER_FORMAT_10, buffer[:10])
                        crc16_expected = None
                    else:
                        _, sequence, sample_count, _, crc16_expected = struct.unpack(HEADER_FORMAT_16, buffer[:16])
                        payload_len = sample_count * 2

                    packet_total_len = hdr_size + payload_len

                    if len(buffer) < packet_total_len:
                        break

                    payload = bytes(buffer[hdr_size:packet_total_len])

                    if header_type_detected == 16 and not args.no_crc and crc16_expected is not None:
                        if crc16_ccitt(payload) != crc16_expected:
                            crc_errors += 1
                            del buffer[:1]
                            continue

                    recv_time = time.time()

                    if expected_sequence is not None and sequence != expected_sequence:
                        gap = (sequence - expected_sequence) & 0xFFFFFFFF
                        dropped_packets += gap
                    expected_sequence = (sequence + 1) & 0xFFFFFFFF

                    raw_u16 = np.frombuffer(payload, dtype=np.uint16)
                    raw_adc = np.bitwise_and(raw_u16, 0x0FFF)
                    num_samples = len(raw_adc)

                    batch_start_time = recv_time - (num_samples * sample_interval)
                    sample_indices = np.arange(num_samples)
                    sample_epochs = batch_start_time + (sample_indices * sample_interval)

                    rows = [
                        (
                            datetime.fromtimestamp(sample_epochs[i]).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
                            int(raw_adc[i])
                        )
                        for i in range(num_samples)
                    ]

                    csv_writer.writerows(rows)
                    del buffer[:packet_total_len]
                    total_samples += num_samples
                    report_samples += num_samples

            # Telemetry status report
            now = time.time()
            elapsed_report = now - last_report_time
            if elapsed_report >= 1.0:
                rate_hz = report_samples / elapsed_report
                data_rate_kb = (total_bytes / (now - start_time)) / 1024.0
                print(
                    f"\r[Telemetry] Rate: {rate_hz:8.1f} Hz | Data Rate: {data_rate_kb:6.1f} KB/s | "
                    f"Total Samples: {total_samples:,} | Loss: {dropped_packets} pkts",
                    end="",
                    flush=True
                )
                last_report_time = now
                report_samples = 0

    except KeyboardInterrupt:
        print("\n\n[!] Stopping acquisition...")
    finally:
        csv_file.flush()
        csv_file.close()
        ser.close()
        elapsed_total = time.time() - start_time
        avg_rate = total_samples / elapsed_total if elapsed_total > 0 else 0
        print("\n==================================================")
        print("                  Summary Report                  ")
        print("==================================================")
        print(f" Total Elapsed Time : {elapsed_total:.2f} seconds")
        print(f" Total Bytes Read   : {total_bytes:,} bytes")
        print(f" Total Samples      : {total_samples:,}")
        print(f" Average Data Rate  : {avg_rate:.1f} Hz")
        print(f" Saved CSV File     : {args.output}")
        print("==================================================")

if __name__ == "__main__":
    main()
