import serial
import time
import sys

SERIAL_PORT = '/dev/ttyUSB0'  # Update to your port
BAUD_RATE = 2000000

print("Testing raw hardware throughput (Skipping CSV writes)...")
try:
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    # ser.set_buffer_size(rx=1024*1024)
    ser.flushInput()
except Exception as e:
    print(f"Failed to open port: {e}"); sys.exit()

start_time = time.time()
total_bytes = 0

try:
    # Run a clean 5-second test loop
    while time.time() - start_time < 5:
        # Read whatever is waiting in the hardware buffer instantly
        waiting = ser.in_waiting
        if waiting > 0:
            raw_data = ser.read(waiting)
            total_bytes += len(raw_data)

    avg_kb_s = (total_bytes / 1024) / 5
    print(f"\n--- TEST RESULTS ---")
    print(f"Total Bytes Received: {total_bytes}")
    print(f"Average Throughput: {avg_kb_s:.2f} KB/s")

    if avg_kb_s > 140:
        print("RESULT: Your ESP32 IS sending 80kHz! The previous Python CSV script was the bottleneck.")
    else:
        print("RESULT: The throughput is still low. The ESP32 hardware itself is throttled.")

except KeyboardInterrupt:
    pass
finally:
    ser.close()
