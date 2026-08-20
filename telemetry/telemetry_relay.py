#!/usr/bin/env python3
"""
======================================================================================
80kHz Piezo UDP Telemetry Relay for Telemetry Viewer
======================================================================================
Unwraps 3-byte framed ESP32 UDP packets ([0xAA][MSB][LSB]) and forwards clean:
1. Binary 16-bit samples to Telemetry Viewer (UDP Port 5006)
2. Or ASCII CSV text stream to Telemetry Viewer (UDP Port 5007)
======================================================================================
"""

import socket
import struct
import sys
import argparse

DEFAULT_IN_PORT = 5005
DEFAULT_OUT_PORT = 5006
READINGS_PER_BURST = 400
EXPECTED_PAYLOAD_SIZE = 8 + (READINGS_PER_BURST * 3) + 2

def main():
    parser = argparse.ArgumentParser(description="Piezo UDP Telemetry Relay for Telemetry Viewer")
    parser.add_argument("--in-port", type=int, default=DEFAULT_IN_PORT, help="Incoming ESP32 UDP Port (default: 5005)")
    parser.add_argument("--out-port", type=int, default=DEFAULT_OUT_PORT, help="Outgoing Telemetry Viewer Port (default: 5006)")
    parser.add_argument("--mode", choices=["binary16", "csv"], default="binary16", help="Output format for Telemetry Viewer")
    args = parser.parse_args()

    in_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    in_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    in_sock.bind(("0.0.0.0", args.in_port))

    out_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f"[RELAY] Listening on 0.0.0.0:{args.in_port} -> Forwarding {args.mode} to 127.0.0.1:{args.out_port}...")

    while True:
        try:
            data, addr = in_sock.recvfrom(2048)
            if len(data) != EXPECTED_PAYLOAD_SIZE:
                continue

            magic1, magic2, seq, count = struct.unpack("<BBIH", data[0:8])
            if magic1 != 0xAA or magic2 != 0x55:
                continue

            payload = data[8:8 + (count * 3)]
            
            if args.mode == "binary16":
                # Convert 3-byte samples ([0xAA][MSB][LSB]) to clean 16-bit binary payload ([Sync][Seq][Raw16...])
                out_payload = bytearray(struct.pack("<BBIH", 0xAA, 0x55, seq, count))
                for i in range(count):
                    msb = payload[i * 3 + 1]
                    lsb = payload[i * 3 + 2]
                    adc_val = (msb << 8) | lsb
                    out_payload.extend(struct.pack("<H", adc_val))
                
                out_sock.sendto(out_payload, ("127.0.0.1", args.out_port))

            elif args.mode == "csv":
                # Convert to ASCII lines: "adc_val,voltage"
                csv_lines = []
                for i in range(count):
                    msb = payload[i * 3 + 1]
                    lsb = payload[i * 3 + 2]
                    adc_val = (msb << 8) | lsb
                    v = (adc_val / 4095.0) * 3.3
                    csv_lines.append(f"{adc_val},{v:.3f}\n")
                
                out_sock.sendto("".join(csv_lines).encode("utf-8"), ("127.0.0.1", args.out_port))

        except KeyboardInterrupt:
            print("\n[EXIT] Relay stopped.")
            break

if __name__ == "__main__":
    main()
