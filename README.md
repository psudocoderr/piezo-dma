# High-Speed ESP32 ADC Continuous DMA Streaming & Python Logger

High-speed continuous 12-bit ADC streaming framework for the ESP32 using hardware DMA and binary packet transfer over high-baud serial (2 Mbaud).

## Project Structure

```
esp32_dma_stream/
├── esp32_dma_stream.ino   # ESP32 Continuous ADC DMA Firmware
├── adc_protocol.h         # Shared Packed Binary Header Protocol & CRC Definition
├── adc_receiver.py        # Python Synchronized Binary Receiver & 2-Column CSV Logger
└── README.md              # Project Documentation & Usage Instructions
```

---

## Architecture Flow

```
ESP32 ADC DMA (Hardware Continuous Sampler, e.g. 80 kS/s)
      │
      ▼
adc_continuous_read()
      │
      ▼
Decode TYPE1 DMA words → uint16_t 12-bit ADC samples
      │
      ▼
Fill 1024-sample payload packet
      │
      ▼
Packet Header (Sync: 0x55AA55AA, Sequence, Sample Count, Timestamp, CRC16)
      │
      ▼
Serial.write(header) + Serial.write(samples) @ 2,000,000 baud
      │
      ▼
Python Receiver (adc_receiver.py)
      │
Search 0x55AA55AA Sync Word
      │
Read & Unpack 16-byte Header
      │
Read Payload & Validate CRC16
      │
Microsecond Batch Timestamping
      │
Write CSV (Columns: timestamp_iso, raw_adc)
```

---

## Hardware Pinout & Wiring

| ESP32 Pin | Signal / Connection | Notes |
| :--- | :--- | :--- |
| **GPIO34** | ADC1 Channel 6 | Input analog voltage (0V to 3.3V) |
| **GND** | Signal Ground | Common reference ground |
| **USB/UART** | PC Serial Connection | 2 Mbaud (requires FT232, CP2102, or native ESP32 USB) |

---

## How to Flash the ESP32 Firmware

1. Open Arduino IDE or PlatformIO.
2. Ensure you have ESP32 Board Support installed (`esp32` package v2.x or v3.x with ESP-IDF v5 continuous ADC support).
3. Select your board (e.g. `ESP32 Dev Module`).
4. Set Baud Rate to `2000000` (2 Mbaud).
5. Compile and Upload `esp32_dma_stream.ino` along with `adc_protocol.h`.

*Note: The ESP32 firmware emits raw binary bytes over Serial. Do not use the Arduino Serial Monitor as it will display gibberish binary stream output.*

---

## How to Run the Python Receiver

### Requirements
- Python 3.8+
- `pyserial`
- `numpy`

Install dependencies:
```bash
pip install pyserial numpy
```

### Execution Command

```bash
python3 adc_receiver.py --port /dev/ttyUSB0 --baud 2000000 --sample-rate 80000 --output adc_samples.csv
```

### Command Line Options

```
-p, --port          Serial port device path (default: /dev/ttyUSB0 or COM3)
-b, --baud          Baud rate (default: 2000000)
-o, --output        Output CSV filename (default: adc_samples.csv)
-r, --sample-rate   Sampling rate in Hz (default: 80000.0)
--no-crc            Disable CRC check for maximum throughput performance
```

### Output CSV Format (`adc_samples.csv`)

| timestamp_iso | raw_adc |
| :--- | :--- |
| 2026-08-05 14:00:00.000000 | 2048 |
| 2026-08-05 14:00:00.000012 | 2055 |
| 2026-08-05 14:00:00.000025 | 2041 |

---

## Active Workspace Recommendation
To work with this codebase seamlessly in your environment, set the active workspace directory to:
`/home/lyniks0611/.gemini/antigravity/scratch/esp32_dma_stream`
