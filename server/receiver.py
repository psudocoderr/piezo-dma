#!/usr/bin/env python3
"""
======================================================================================
80kHz Piezo Sensor UDP Binary Receiver & Data Sanity Validator
Target MCU: ESP32 DevKit V1 (ESP32-D0WD / ESP32-WROOM-32)
======================================================================================
"""

import socket
import struct
import sys
import time
import argparse
from datetime import datetime
from pathlib import Path

# --- Configuration Defaults ---
DEFAULT_UDP_IP = "0.0.0.0"
DEFAULT_UDP_PORT = 5005
DEFAULT_CSV_PATH = "piezo_data.csv"
SAMPLE_RATE_HZ = 80000.0
SAMPLE_PERIOD_SEC = 1.0 / SAMPLE_RATE_HZ
READINGS_PER_BURST = 400
SYNC_BYTE = 0xAA

# Payload breakdown:
# Header: 1B (magic1) + 1B (magic2) + 4B (seq) + 2B (count) = 8 bytes
# Payload: 400 readings * 3 bytes/reading = 1,200 bytes
# Footer: 2B (crc16)
# Total UDP Payload Size = 1,210 bytes
EXPECTED_PAYLOAD_SIZE = 8 + (READINGS_PER_BURST * 3) + 2

def format_custom_timestamp(ts: float) -> str:
    dt = datetime.fromtimestamp(ts)
    ms = dt.microsecond // 1000
    f6_str = f"{dt.microsecond:06d}"
    date_str = dt.strftime("%d-%m-%Y %H:%M:%S")
    return f"{date_str}:{ms:03d}.{f6_str}"

# Precomputed CRC16-CCITT 256-entry lookup table for high performance (200 pkts/sec)
CRC16_TABLE = []
for _i in range(256):
    _curr = _i << 8
    for _ in range(8):
        if _curr & 0x8000:
            _curr = ((_curr << 1) ^ 0x1021) & 0xFFFF
        else:
            _curr = (_curr << 1) & 0xFFFF
    CRC16_TABLE.append(_curr)

def calculate_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) ^ CRC16_TABLE[((crc >> 8) ^ byte) & 0xFF]) & 0xFFFF
    return crc

class PiezoUDPReceiver:
    def __init__(self, ip: str, port: int, csv_filepath: str, verbose: bool = False):
        self.ip = ip
        self.port = port
        self.csv_filepath = Path(csv_filepath)
        self.verbose = verbose

        self.expected_seq = None
        self.total_packets_received = 0
        self.total_samples_logged = 0
        self.crc_errors = 0
        self.dropped_packets = 0
        self.sync_errors = 0
        self.sample_sync_errors = 0
        self.clipping_samples = 0
        self.flatline_samples = 0

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.bind((self.ip, self.port))
        self.sock.settimeout(2.0)  # Periodic timeout to log waiting status

    def run(self):
        print(f"[INIT] Listening for 80kHz UDP binary burst stream on {self.ip}:{self.port}...")
        print(f"[INIT] Saving output to CSV: {self.csv_filepath.resolve()}")

        # Ensure parent directory exists
        self.csv_filepath.parent.mkdir(parents=True, exist_ok=True)

        with open(self.csv_filepath, mode="w", buffering=65536, encoding="utf-8") as csv_file:
            csv_file.write("ts,adc_raw\n")

            last_report_time = time.time()
            start_stream_time = None
            first_packet_logged = False

            try:
                while True:
                    try:
                        data, addr = self.sock.recvfrom(2048)
                    except socket.timeout:
                        if self.total_packets_received == 0:
                            print(f"[WAITING] Waiting for ESP32 UDP packets on port {self.port}... (No data received yet)")
                        continue

                    if not first_packet_logged:
                        print(f"[CONNECTED] First UDP packet received from {addr[0]}:{addr[1]} ({len(data)} bytes)")
                        first_packet_logged = True

                    if len(data) != EXPECTED_PAYLOAD_SIZE:
                        print(f"[WARNING] Mismatch packet size from {addr[0]}: expected {EXPECTED_PAYLOAD_SIZE} bytes, got {len(data)} bytes")
                        self.sync_errors += 1
                        continue

                    magic1, magic2, seq, count = struct.unpack("<BBIH", data[0:8])

                    if magic1 != 0xAA or magic2 != 0x55:
                        print(f"[ERROR] Magic byte mismatch from {addr[0]}: 0x{magic1:02X} 0x{magic2:02X} (Expected 0xAA 0x55)")
                        self.sync_errors += 1
                        continue

                    crc_received = struct.unpack("<H", data[-2:])[0]
                    crc_calculated = calculate_crc16(data[:-2])

                    if crc_received != crc_calculated:
                        print(f"[ERROR] Packet {seq} from {addr[0]}: CRC Mismatch! Received: 0x{crc_received:04X}, Calc: 0x{crc_calculated:04X}")
                        self.crc_errors += 1
                        continue

                    if self.expected_seq is not None:
                        if seq != self.expected_seq:
                            lost = (seq - self.expected_seq) & 0xFFFFFFFF
                            self.dropped_packets += lost

                    self.expected_seq = (seq + 1) & 0xFFFFFFFF
                    recv_time = time.time()
                    if start_stream_time is None:
                        start_stream_time = recv_time

                    payload = data[8:8 + (count * 3)]
                    adc_samples = []

                    for i in range(count):
                        sync = payload[i * 3]
                        msb = payload[i * 3 + 1]
                        lsb = payload[i * 3 + 2]

                        if sync != SYNC_BYTE:
                            self.sample_sync_errors += 1

                        adc_val = (msb << 8) | lsb
                        adc_samples.append(adc_val)

                        if adc_val >= 4090:
                            self.clipping_samples += 1
                        elif adc_val <= 5:
                            self.flatline_samples += 1

                    base_ts = recv_time - (count * SAMPLE_PERIOD_SEC)
                    csv_lines = []
                    for i, adc_val in enumerate(adc_samples):
                        sample_ts = base_ts + (i * SAMPLE_PERIOD_SEC)
                        ts_formatted = format_custom_timestamp(sample_ts)
                        csv_lines.append(f"{ts_formatted},{adc_val}\n")

                    csv_file.writelines(csv_lines)
                    self.total_packets_received += 1
                    self.total_samples_logged += count

                    now = time.time()
                    if now - last_report_time >= 2.0:
                        elapsed = now - start_stream_time
                        actual_rate = self.total_samples_logged / elapsed if elapsed > 0 else 0
                        print(f"[STATUS] Logged: {self.total_samples_logged:,} samples | Rate: {actual_rate:,.1f} Hz | Packets: {self.total_packets_received:,}")
                        last_report_time = now

            except KeyboardInterrupt:
                print("\n[EXIT] Collection stopped by user.")
                print(f"[SUMMARY] Total Samples Written : {self.total_samples_logged:,}")

def main():
    parser = argparse.ArgumentParser(description="80kHz Piezo UDP Receiver")
    parser.add_argument("--ip", default=DEFAULT_UDP_IP, help="UDP listen IP")
    parser.add_argument("--port", type=int, default=DEFAULT_UDP_PORT, help="UDP listen port")
    parser.add_argument("--csv", default=DEFAULT_CSV_PATH, help="Output CSV path")
    parser.add_argument("--verbose", action="store_true", help="Print debug logs")
    args = parser.parse_args()

    receiver = PiezoUDPReceiver(args.ip, args.port, args.csv, args.verbose)
    receiver.run()

if __name__ == "__main__":
    main()
