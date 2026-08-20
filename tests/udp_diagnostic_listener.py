#!/usr/bin/env python3
"""
======================================================================================
80kHz Piezo UDP Network & Packet Diagnostic Listener (Standalone File)
======================================================================================
This is a standalone diagnostic tool to locate incoming ESP32 UDP packets,
display local IP configurations, and test network throughput without modifying
any existing receiver or server files.
======================================================================================
"""

import socket
import struct
import sys
import time
import argparse
from datetime import datetime

DEFAULT_PORT = 5005
READINGS_PER_BURST = 400
EXPECTED_PAYLOAD_SIZE = 8 + (READINGS_PER_BURST * 3) + 2

def get_local_ip_addresses():
    """Returns a list of local IP addresses for this machine."""
    ip_list = []
    try:
        # Create a dummy socket to detect primary outgoing interface IP
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        primary_ip = s.getsockname()[0]
        s.close()
        ip_list.append(primary_ip)
    except Exception:
        pass

    try:
        hostname = socket.gethostname()
        for ip in socket.gethostbyname_ex(hostname)[2]:
            if ip not in ip_list and not ip.startswith("127."):
                ip_list.append(ip)
    except Exception:
        pass

    return ip_list

def send_test_loopback_packet(target_ip="127.0.0.1", target_port=DEFAULT_PORT):
    """Sends a dummy UDP burst packet to test listener functionality locally."""
    print(f"[TEST TX] Sending test UDP packet to {target_ip}:{target_port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # Construct header: magic1 (0xAA), magic2 (0x55), seq (1), count (400)
    header = struct.pack("<BBIH", 0xAA, 0x55, 1, READINGS_PER_BURST)
    
    # Construct 400 sample payload: [0xAA, MSB, LSB] -> 2048 (1.65V)
    reading = struct.pack("<BBB", 0xAA, 0x08, 0x00)
    payload = reading * READINGS_PER_BURST
    
    # Dummy CRC16
    crc = struct.pack("<H", 0x1234)
    
    packet = header + payload + crc
    sock.sendto(packet, (target_ip, target_port))
    sock.close()
    print(f"[TEST TX] Sent {len(packet)} bytes test packet.")

def run_diagnostic_listener(port: int, test_tx: bool):
    local_ips = get_local_ip_addresses()
    print("\n==========================================================")
    print(" 🛠️  ESP32-S3 80kHz UDP Network Diagnostic Listener")
    print("==========================================================")
    print(f"[NET INFO] Local Host IPs detected: {', '.join(local_ips) if local_ips else 'Unknown'}")
    print(f"[NET INFO] Check secrets.h in firmware to match one of the IPs above!")
    print(f"[NET INFO] Listening on ALL interfaces (0.0.0.0:{port})...\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(2.0)

    if test_tx:
        time.sleep(0.5)
        send_test_loopback_packet("127.0.0.1", port)

    total_pkts = 0
    start_time = None
    last_seq = None

    try:
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                if total_pkts == 0:
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] Listening... No UDP packets received yet on port {port}.")
                continue

            recv_time = time.time()
            if start_time is None:
                start_time = recv_time

            total_pkts += 1

            if len(data) != EXPECTED_PAYLOAD_SIZE:
                print(f"[PACKET #{total_pkts}] From {addr[0]}:{addr[1]} | Size Mismatch: {len(data)} bytes (Expected {EXPECTED_PAYLOAD_SIZE})")
                continue

            magic1, magic2, seq, count = struct.unpack("<BBIH", data[0:8])
            
            # Extract first sample for quick display
            sync0, msb0, lsb0 = struct.unpack("<BBB", data[8:11])
            adc_val0 = (msb0 << 8) | lsb0
            v0 = (adc_val0 / 4095.0) * 3.3

            drops = ""
            if last_seq is not None and seq != last_seq + 1:
                dropped_cnt = (seq - (last_seq + 1)) & 0xFFFFFFFF
                drops = f" | [DROPPED: {dropped_cnt}]"
            last_seq = seq

            if total_pkts == 1 or total_pkts % 200 == 0:
                elapsed = recv_time - start_time
                pkt_rate = total_pkts / elapsed if elapsed > 0 else 0
                sample_rate = pkt_rate * count
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Pkt #{seq} from {addr[0]}:{addr[1]} | "
                      f"Sample0: {adc_val0} ({v0:.2f}V) | Pkts/sec: {pkt_rate:.1f} | Sample Rate: {sample_rate:,.0f} Hz{drops}")

    except KeyboardInterrupt:
        print("\n[EXIT] Diagnostic listener stopped by user.")

def main():
    parser = argparse.ArgumentParser(description="Standalone UDP Network Diagnostic Listener")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="UDP port to listen on (default: 5005)")
    parser.add_argument("--test-tx", action="store_true", help="Send a test packet to localhost to verify listener")
    args = parser.parse_args()

    run_diagnostic_listener(args.port, args.test_tx)

if __name__ == "__main__":
    main()
