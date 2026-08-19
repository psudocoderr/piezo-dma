#!/usr/bin/env python3
"""
High-Speed ESP32 Binary ADC Receiver & CSV Logger
Reads 16-bit binary continuous ADC blocks from ESP32 over Serial (2 Mbaud),
converts bytes [High Byte, Low Byte] into raw ADC values and analog voltages,
applies microsecond-accurate batch timestamps, and logs samples to a local CSV file.
"""
import sys
import time
import argparse
import csv
from datetime import datetime
import serial
import numpy as np
def parse_args():
    parser = argparse.ArgumentParser(description="ESP32 High-Speed ADC Binary Data Receiver & CSV Logger")
    parser.add_argument("-p", "--port", type=str, default="/dev/ttyUSB0", help="Serial port (e.g. /dev/ttyUSB0 or COM3)")
    parser.add_argument("-b", "--baud", type=int, default=2000000, help="Baud rate (default: 2000000)")
    parser.add_argument("-o", "--output", type=str, default="adc_samples.csv", help="Output CSV file path")
    parser.add_argument("-r", "--sample-rate", type=float, default=80000.0, help="Expected sampling rate in Hz (default: 80000)")
    parser.add_argument("-v", "--vref", type=float, default=3.3, help="ADC reference voltage in Volts (default: 3.3V)")
    return parser.parse_args()
def main():
    args = parse_args()

    print("==================================================")
    print("      ESP32 ADC High-Speed Binary Receiver       ")
    print("==================================================")
    print(f" Port            : {args.port}")
    print(f" Baud Rate       : {args.baud} bps")
    print(f" Sampling Rate   : {args.sample_rate:.0f} Hz")
    print(f" ADC Ref Voltage : {args.vref} V")
    print(f" CSV Output      : {args.output}")
    print("==================================================")
    # Open serial connection
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1.0)
        ser.reset_input_buffer()
        print(f"[+] Successfully opened {args.port} at {args.baud} baud.")
    except Exception as e:
        print(f"[!] Error opening serial port {args.port}: {e}")
        print("    Please check device connection or permissions (e.g., sudo chmod 666 /dev/ttyUSB0).")
        sys.exit(1)
    # Open CSV file for writing
    try:
        csv_file = open(args.output, mode="w", newline="", buffering=1024 * 1024)
        csv_writer = csv.writer(csv_file)
        # CSV Header
        csv_writer.writerow(["timestamp_iso", "epoch_time_sec", "sample_index", "raw_adc", "voltage_v"])
        print(f"[+] Output CSV file '{args.output}' initialized.")
    except Exception as e:
        print(f"[!] Error opening output file {args.output}: {e}")
        ser.close()
        sys.exit(1)
    # Processing State & Counters
    sample_interval = 1.0 / args.sample_rate
    bytes_per_sample = 2
    chunk_samples = 1024
    chunk_bytes = chunk_samples * bytes_per_sample

    total_samples = 0
    total_bytes = 0
    start_time = time.time()
    last_report_time = start_time
    report_samples = 0
    print("\n[>] Continuous logging started. Press Ctrl+C to stop.\n")
    try:
        while True:
            # Read binary chunk from serial stream
            raw_data = ser.read(chunk_bytes)
            if not raw_data:
                continue
            batch_recv_time = time.time()
            num_bytes = len(raw_data)
            num_samples = num_bytes // bytes_per_sample
            if num_samples == 0:
                continue
            # Batch timestamping: calculate precise sample times anchored to batch arrival time
            # sample_time_i = batch_recv_time - (num_samples - i) * sample_interval
            batch_start_sample_time = batch_recv_time - (num_samples * sample_interval)
            # Vectorized fast byte parsing using NumPy
            raw_u16 = np.frombuffer(raw_data, dtype=np.uint16)
            raw_adc = np.bitwise_and(raw_u16, 0x0FFF)
            voltages = raw_adc * (args.vref / 4095.0)

            # Vectorized timestamps & sample indices
            indices = np.arange(num_samples)
            sample_epochs = batch_start_sample_time + (indices * sample_interval)
            global_indices = total_samples + indices

            # Fast row formatting
            rows = [
                (
                    datetime.fromtimestamp(sample_epochs[i]).strftime("%Y-%m-%d %H:%M:%S.%f"),
                    f"{sample_epochs[i]:.6f}",
                    int(global_indices[i]),
                    int(raw_adc[i]),
                    f"{voltages[i]:.4f}",
                )
                for i in range(num_samples)
            ]

            # Fast batch write to CSV
            csv_writer.writerows(rows)
            total_samples += num_samples
            report_samples += num_samples
            total_bytes += num_bytes
            # Telemetry reporting every 1 second
            now = time.time()
            elapsed_report = now - last_report_time
            if elapsed_report >= 1.0:
                rate_hz = report_samples / elapsed_report
                data_rate_kb = (total_bytes / (now - start_time)) / 1024.0
                print(f"\r[Telemetry] Logging Rate: {rate_hz:8.1f} Hz | Data Rate: {data_rate_kb:6.1f} KB/s | Total Samples: {total_samples:,}", end="", flush=True)
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
        print(f" Total Samples      : {total_samples:,}")
        print(f" Average Data Rate  : {avg_rate:.1f} Hz")
        print(f" Saved CSV File     : {args.output}")
        print("==================================================")
if __name__ == "__main__":
    main()
