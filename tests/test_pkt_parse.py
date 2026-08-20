#!/usr/bin/env python3
import struct

# Raw bytes from tcpdump
payload_hex = "aa55d77100009001" + ("aa0000" * 400) + "d78a"
data = bytes.fromhex(payload_hex)

EXPECTED_PAYLOAD_SIZE = 8 + (400 * 3) + 2

print(f"Len: {len(data)}, Expected: {EXPECTED_PAYLOAD_SIZE}")

magic1, magic2, seq, count = struct.unpack("<BBIH", data[0:8])
print(f"magic1: 0x{magic1:02X}, magic2: 0x{magic2:02X}, seq: {seq}, count: {count}")

crc_received = struct.unpack("<H", data[-2:])[0]

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

crc_calculated = calculate_crc16(data[:-2])

print(f"CRC Rec: 0x{crc_received:04X}, Calc: 0x{crc_calculated:04X}")
