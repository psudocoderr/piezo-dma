#!/usr/bin/env python3
import struct

# Hex dump from tcpdump packet:
# Header (8 bytes): aa55 d771 0000 9001 -> magic1=0xAA, magic2=0x55, seq=29143, count=400
# Followed by 400 * 3 bytes = 1200 bytes of payload (all 0xAA 0x00 0x00)
# Footer (2 bytes): d78a

payload_hex = "aa55d77100009001" + ("aa0000" * 400) + "d78a"
data = bytes.fromhex(payload_hex)

print(f"Total length: {len(data)} (expected 1210)")

def calculate_crc16_python(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

crc_rec = struct.unpack("<H", data[-2:])[0]
crc_calc = calculate_crc16_python(data[:-2])

print(f"CRC Received (unpack '<H'): 0x{crc_rec:04X}")
print(f"CRC Calculated in Python  : 0x{crc_calc:04X}")

# Let's also check big endian unpack '>H'
crc_rec_be = struct.unpack(">H", data[-2:])[0]
print(f"CRC Received (unpack '>H'): 0x{crc_rec_be:04X}")
