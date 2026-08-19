# import serial
# import numpy as np
#
# ser = serial.Serial("/dev/ttyUSB0", 2000000)
#
# while True:
#     data = ser.read(2048)           # 1024 samples × 2 bytes
#     samples = np.frombuffer(data, dtype=np.uint16) & 0x0FFF
#     print(samples[:10])

import serial
import struct
import csv
import time


PORT = "/dev/ttyUSB0"      # change this
BAUD = 2000000

SYNC = 0xAA

OUTPUT = "adc_capture.csv"


ser = serial.Serial(
    PORT,
    BAUD,
    timeout=2
)


print("Opening:", PORT)


with open(OUTPUT, "w", newline="") as f:

    writer = csv.writer(f)

    writer.writerow([
        "time_ms",
        "adc"
    ])


    start_time = time.time()


    sample_index = 0


    while True:

        # --------------------------
        # Find sync byte
        # --------------------------
        b = ser.read(1)

        if not b:
            continue

        if b[0] != SYNC:
            continue


        # --------------------------
        # Read sample count
        # --------------------------
        count_bytes = ser.read(2)

        if len(count_bytes) != 2:
            continue


        count = struct.unpack(
            "<H",
            count_bytes
        )[0]


        # --------------------------
        # Read ADC data
        # --------------------------
        data = ser.read(count * 2)


        if len(data) != count * 2:
            continue


        samples = struct.unpack(
            "<" + "H" * count,
            data
        )


        # --------------------------
        # Write CSV
        # --------------------------
        for value in samples:

            value &= 0x0FFF

            t = (
                sample_index *
                1000000 /
                80000
            )

            writer.writerow([
                t / 1000,
                value
            ])

            sample_index += 1


        print(
            "Samples:",
            sample_index,
            "Last:",
            samples[-1]
        )
