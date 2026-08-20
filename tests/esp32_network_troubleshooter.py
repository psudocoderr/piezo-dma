#!/usr/bin/env python3
"""
======================================================================================
ESP32-S3 Network & UDP Packet Receiver Troubleshooter (Standalone File)
======================================================================================
This script performs system-level network checks to identify why UDP packets 
from the XIAO ESP32-S3 are not reaching the host machine.
======================================================================================
"""

import socket
import struct
import sys
import time
import subprocess
from datetime import datetime

PORT = 5005

def check_local_network_interfaces():
    print("----------------------------------------------------------")
    print("1. LOCAL NETWORK INTERFACE CHECK")
    print("----------------------------------------------------------")
    try:
        # Run ip route / ifconfig
        res = subprocess.run(["ip", "-4", "addr", "show"], capture_output=True, text=True)
        print(res.stdout)
    except Exception as e:
        print(f"Unable to run ip command: {e}")

def check_firewall_status():
    print("----------------------------------------------------------")
    print("2. FIREWALL STATUS CHECK (Port 5005)")
    print("----------------------------------------------------------")
    try:
        res = subprocess.run(["sudo", "ufw", "status"], capture_output=True, text=True)
        print(res.stdout)
    except Exception:
        print("[INFO] ufw check skipped (requires root or ufw not installed).")

def listen_udp_all_interfaces():
    print("----------------------------------------------------------")
    print("3. LIVE UDP PACKET LISTENER (0.0.0.0:5005)")
    print("----------------------------------------------------------")
    print(f"Listening for incoming UDP packets on port {PORT}...")
    print("Press Ctrl+C to stop.\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    
    try:
        sock.bind(("0.0.0.0", PORT))
    except Exception as e:
        print(f"[ERROR] Failed to bind to 0.0.0.0:{PORT}: {e}")
        return

    sock.settimeout(2.0)
    pkt_count = 0

    while True:
        try:
            data, addr = sock.recvfrom(2048)
            pkt_count += 1
            now_str = datetime.now().strftime("%H:%M:%S")
            print(f"[{now_str}] SUCCESS! Received {len(data)} bytes from ESP32 IP: {addr[0]}:{addr[1]}")
            
            if len(data) >= 8:
                magic1, magic2, seq, count = struct.unpack("<BBIH", data[0:8])
                print(f"         Header -> Magic: 0x{magic1:02X} 0x{magic2:02X} | Seq: {seq} | Samples: {count}")
        except socket.timeout:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Waiting for ESP32 packets... (0 packets received so far)")
        except KeyboardInterrupt:
            print("\nStopped by user.")
            break

if __name__ == "__main__":
    check_local_network_interfaces()
    check_firewall_status()
    listen_udp_all_interfaces()
