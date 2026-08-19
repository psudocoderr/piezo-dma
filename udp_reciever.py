#!/usr/bin/env python3
"""Validate and decode ESP32 piezo UDP packets."""

from __future__ import annotations

import argparse
import socket
import struct
import time

SYNC = b"\xA5\x5A"
VERSION = 1
HEADER_BYTES = 20
CRC_BYTES = 2


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (
                crc << 1
            ) & 0xFFFF
    return crc


def decode(packet: bytes):
    if len(packet) < HEADER_BYTES + CRC_BYTES:
        return None
    if packet[:2] != SYNC or packet[2] != VERSION:
        return None

    sequence, sample_rate, first_sample, sample_count, payload_bytes = struct.unpack_from(
        "<IIIHH", packet, 4
    )
    expected_length = HEADER_BYTES + payload_bytes + CRC_BYTES
    if payload_bytes != sample_count * 2 or expected_length != len(packet):
        return None

    received_crc = struct.unpack_from("<H", packet, HEADER_BYTES + payload_bytes)[0]
    if crc16_ccitt_false(packet[: HEADER_BYTES + payload_bytes]) != received_crc:
        return None

    samples = struct.unpack_from(f"<{sample_count}H", packet, HEADER_BYTES)
    return sequence, sample_rate, first_sample, samples


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5005)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    sock.bind((args.bind, args.port))
    print(f"Listening on {args.bind}:{args.port}")

    packets = bad_packets = packet_gaps = sample_gaps = 0
    previous_sequence = previous_last_sample = None
    started = time.monotonic()

    while True:
        datagram, address = sock.recvfrom(2048)
        decoded = decode(datagram)
        if decoded is None:
            bad_packets += 1
            continue

        sequence, sample_rate, first_sample, samples = decoded
        packets += 1
        if previous_sequence is not None and sequence != previous_sequence + 1:
            packet_gaps += (sequence - previous_sequence - 1) & 0xFFFFFFFF
        if previous_last_sample is not None and first_sample != previous_last_sample:
            sample_gaps += abs(first_sample - previous_last_sample)
        previous_sequence = sequence
        previous_last_sample = first_sample + len(samples)

        if packets == 1 or packets % 100 == 0:
            elapsed = max(time.monotonic() - started, 1e-9)
            rate = (previous_last_sample or 0) / elapsed
            print(
                f"{address[0]}  packets={packets} bad={bad_packets} "
                f"packet_gaps={packet_gaps} sample_gaps={sample_gaps} "
                f"sample_rate={sample_rate}Hz measured={rate:,.0f}Hz "
                f"range={min(samples)}..{max(samples)}"
            )


if __name__ == "__main__":
    main()
