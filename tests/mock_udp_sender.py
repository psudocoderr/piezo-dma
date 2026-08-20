#!/usr/bin/env python3
"""
======================================================================================
Mock 80kHz UDP Sender (3-Byte Binary Sample Format)
Target MCU: ESP32 DevKit V1 Simulation
======================================================================================
Simulates an ESP32 sending binary burst frames over UDP at 80,000 Hz.
Each reading consists of 3 bytes: [0xAA Sync][MSB][LSB].
Used for end-to-end data sanity verification and CSV generator testing.
======================================================================================
"""

import math
import socket
import struct
import time
import argparse

SYNC_BYTE = 0xAA
READINGS_PER_BURST = 400


def calculate_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def run_mock_sender(target_ip: str, target_port: int, duration_sec: float, inject_errors: bool = False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    sample_rate = 80000.0
    burst_rate_hz = sample_rate / READINGS_PER_BURST  # 200.0 Hz (5ms per burst)
    burst_interval_sec = 1.0 / burst_rate_hz

    print(f"[MOCK SENDER] Transmitting 80kHz Piezo stream to {target_ip}:{target_port} for {duration_sec}s...")
    print(f"[MOCK SENDER] Rate: {burst_rate_hz:.2f} bursts/sec ({READINGS_PER_BURST} readings/packet, 3 bytes/reading)")

    seq = 0
    t_global = 0.0
    start_time = time.time()

    while (time.time() - start_time) < duration_sec:
        burst_start = time.time()

        # Generate 400 3-byte formatted piezo ADC readings
        payload_bytes = bytearray()
        for _ in range(READINGS_PER_BURST):
            # Simulating a 5 kHz piezo vibration with 12-bit ADC range (0..4095)
            piezo_signal = 2048 + int(1400 * math.sin(2 * math.pi * 5000 * t_global))
            piezo_val = max(0, min(4095, piezo_signal))

            msb = (piezo_val >> 8) & 0xFF
            lsb = piezo_val & 0xFF

            # Append 3 bytes: [0xAA Sync][MSB][LSB]
            payload_bytes.append(SYNC_BYTE)
            payload_bytes.append(msb)
            payload_bytes.append(lsb)

            t_global += 1.0 / sample_rate

        current_seq = seq
        if inject_errors and seq == 50:
            print("[TEST INJECT] Simulating dropped UDP packet (skipping seq 50)")
            seq += 1
            current_seq = seq

        # Header: magic1(0xAA), magic2(0x55), seq(uint32), count(uint16=400)
        header = struct.pack("<BBIH", 0xAA, 0x55, current_seq, READINGS_PER_BURST)
        frame_data = header + payload_bytes

        # Calculate CRC16 checksum
        crc = calculate_crc16(frame_data)
        if inject_errors and seq == 100:
            print("[TEST INJECT] Simulating CRC corruption on seq 100")
            crc ^= 0xFFFF

        packet = frame_data + struct.pack("<H", crc)

        # Send UDP packet
        sock.sendto(packet, (target_ip, target_port))
        seq += 1

        # Throttle timing
        elapsed = time.time() - burst_start
        sleep_time = burst_interval_sec - elapsed
        if sleep_time > 0:
            time.sleep(sleep_time)

    print(f"[MOCK SENDER] Complete. Sent {seq} packets ({seq * READINGS_PER_BURST:,} samples).")


def main():
    parser = argparse.ArgumentParser(description="Mock 80kHz UDP Generator (3-Byte Sample Format)")
    parser.add_argument("--ip", default="127.0.0.1", help="Target IP address")
    parser.add_argument("--port", type=int, default=5005, help="Target UDP Port")
    parser.add_argument("--duration", type=float, default=5.0, help="Duration in seconds")
    parser.add_argument("--inject-errors", action="store_true", help="Inject test sequence drops and CRC errors")
    args = parser.parse_args()

    run_mock_sender(args.ip, args.port, args.duration, args.inject_errors)


if __name__ == "__main__":
    main()
