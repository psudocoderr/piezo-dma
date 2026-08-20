# 80kHz Piezo Sensor DMA & UDP Transport Engine (v1.0)

High-performance, ultra-low-latency 80,000 Hz continuous acquisition system for piezoelectric sensors using ESP32 Hardware DMA and UDP Wi-Fi burst streaming.

---

## 📖 Table of Contents
1. [Theory of Operation](#-theory-of-operation)
2. [Repository & Directory Structure](#-repository--directory-structure)
3. [Binary Protocol Specification](#-binary-protocol-specification)
4. [Commands & Execution Guide](#-commands--execution-guide)
5. [Telemetry & Web Visualizer](#-telemetry--web-visualizer)
6. [Testing & Diagnostic Suite](#-testing--diagnostic-suite)
7. [Comprehensive Troubleshooting Guide](#-comprehensive-troubleshooting-guide)

---

## 🔬 Theory of Operation

Acquiring continuous high-frequency analogue signals (80 kHz) without sample dropouts requires hardware-level DMA (Direct Memory Access) offloading from the CPU:

```
[ Piezo Sensor ] ──> [ ESP32 GPIO 34 / SPI ] ──> [ Hardware DMA Engine ]
                                                         │ (Ping-Pong Buffer)
                                                         ▼
[ Host CSV Server ] <── [ UDP Burst Stream ] <── [ FreeRTOS Task ]
```

1. **Hardware DMA Acquisition**:
   - **Internal ADC Mode (`MODE_I2S_ADC_DMA`)**: Leverages the ESP32 `I2S0` peripheral in built-in ADC mode. The DMA controller autonomously samples GPIO 34 (`ADC1_CHANNEL_6`) at exactly 80,000 Hz into double-buffered (ping-pong) descriptors without CPU intervention.
   - **SPI ADC Mode (`MODE_SPI_DMA`)**: Uses VSPI DMA for external high-speed ADCs (e.g., MAX11105 / MCP3201 / ADS7883) at up to 10 MHz SPI clock rate.

2. **3-Byte Sample Framing**:
   Every 12-bit raw ADC reading is encapsulated into a 3-byte framed structure:
   - `0xAA` Sync marker byte
   - `MSB` (Upper 8 bits)
   - `LSB` (Lower 8 bits)
   This guaranteed framing enables mid-stream packet synchronization and frame boundary validation.

3. **Non-Blocking UDP Burst Streaming**:
   Samples are aggregated into 400-reading bursts (1,200 payload bytes). A total datagram of **1,210 bytes** (Header + Payload + CRC16) is transmitted via Wi-Fi UDP at 200 packets per second (80,000 samples/sec). UDP overhead is minimized while staying safely under the standard 1,500-byte Ethernet MTU.

4. **Data Sanity & Verification Engine**:
   - **Sequence Number Tracking**: Detects dropped Wi-Fi packets and calculates real-time packet loss percentage.
   - **CRC16-CCITT Checksum**: 16-bit CRC computed across the header and payload to drop corrupted Wi-Fi datagrams.
   - **Rail Saturation & Flatline Monitoring**: Automatic flags when ADC values hit 4095 (high-rail saturation) or <= 5 (sensor disconnection).

---

## 📁 Repository & Directory Structure

```
piezo-dma/
├── .gitignore                      # Git ignore rules (filters python bytecode, binaries, secrets)
├── README.md                       # Comprehensive operational & protocol manual
├── firmware/
│   └── firmware_esp32_dma_udp/
│       ├── firmware_esp32_dma_udp.ino   # Core 80kHz DMA UDP C++ firmware (ESP32 DevKit V1)
│       ├── secrets.h.example            # Wi-Fi SSID & UDP IP template
│       └── secrets.h                    # Private credentials (git-ignored)
├── server/
│   └── receiver.py                 # Main 80kHz UDP Receiver, Validator & CSV Logger
├── telemetry/
│   ├── telemetry_web_server.py     # Real-time Web Dashboard (HTTP static server)
│   ├── telemetry_relay.py          # High-speed WebSocket relay engine
│   └── telemetry_viewer_config.md  # Setup guide for 3rd-party Telemetry Viewer GUI
└── tests/
    ├── test_crc_diag.py            # Standalone CRC16 verification script
    ├── test_pkt_parse.py           # Packet parser unit test suite
    ├── udp_diagnostic_listener.py  # Real-time UDP network packet inspector
    ├── mock_udp_sender.py          # Synthetic 80kHz burst stream generator
    ├── esp32_network_troubleshooter.py # Wi-Fi latency & socket ping tool
    └── serial_piezo_receiver.py    # Fallback high-speed Serial binary receiver (3 Mbaud)
```

---

## 🛰️ Binary Protocol Specification

### Total UDP Datagram Size: 1,210 Bytes

#### 1. Burst Header (8 Bytes, Little-Endian)
| Offset | Field | Type | Value / Description |
| :--- | :--- | :--- | :--- |
| `0x00` | `magic1` | `uint8_t` | `0xAA` (Protocol Magic Byte 1) |
| `0x01` | `magic2` | `uint8_t` | `0x55` (Protocol Magic Byte 2) |
| `0x02` | `sequence_num` | `uint32_t` | Monotonic Packet Sequence ID |
| `0x06` | `sample_count` | `uint16_t` | `400` (Number of 3-byte readings) |

#### 2. Payload (1,200 Bytes)
Repeated `400` times:
| Field | Type | Description |
| :--- | :--- | :--- |
| `sync` | `uint8_t` | `0xAA` Sample Sync Marker |
| `msb` | `uint8_t` | High 8 bits of ADC raw value `(val >> 8) & 0xFF` |
| `lsb` | `uint8_t` | Low 8 bits of ADC raw value `val & 0xFF` |

#### 3. Footer (2 Bytes)
| Offset | Field | Type | Description |
| :--- | :--- | :--- | :--- |
| `1208` | `crc16` | `uint16_t` | CRC16-CCITT polynomial `0x1021`, init `0xFFFF` |

---

## 🚀 Commands & Execution Guide

### 1. Firmware Setup (ESP32 DevKit V1)
1. Copy `firmware/firmware_esp32_dma_udp/secrets.h.example` to `secrets.h`:
   ```bash
   cp firmware/firmware_esp32_dma_udp/secrets.h.example firmware/firmware_esp32_dma_udp/secrets.h
   ```
2. Edit `secrets.h` with your Wi-Fi SSID, password, and host IP address:
   ```cpp
   #define WIFI_SSID     "Your_WiFi_SSID"
   #define WIFI_PASSWORD "Your_WiFi_Password"
   #define UDP_DEST_IP   "192.168.1.100"  // Host PC IP address
   #define UDP_DEST_PORT 5005
   ```
3. Open `firmware/firmware_esp32_dma_udp/firmware_esp32_dma_udp.ino` in Arduino IDE or VS Code PlatformIO.
4. Select Board: **ESP32 Dev Module** (ESP32-D0WD / DevKit V1).
5. Flash firmware and open Serial Monitor at **3,000,000 baud** (or 115,200 for boot text).

### 2. Main Receiver & CSV Logging Server
Run the primary receiver server on the Host PC:
```bash
python3 server/receiver.py --ip 0.0.0.0 --port 5005 --csv piezo_data.csv --verbose
```
* **Output Format**:
  ```csv
  ts,adc_raw
  20-08-2026 13:45:00:123.123456,2048
  20-08-2026 13:45:00:123.135956,2054
  ```

---

## 📊 Telemetry & Web Visualizer

### 1. Running Web Telemetry Server
Start the real-time telemetry server:
```bash
# Terminal 1: Launch WebSocket Relay
python3 telemetry/telemetry_relay.py --port 5005 --ws-port 8081

# Terminal 2: Launch Web Server Dashboard
python3 telemetry/telemetry_web_server.py --port 8080
```
Navigate to `http://localhost:8080` in your web browser.

### 2. 3rd-Party Telemetry Viewer Setup
Refer to [telemetry_viewer_config.md](file:///home/lyniks0611/Cloud/OneDrive02/Documents/Arduino/piezo-dma/telemetry/telemetry_viewer_config.md) for step-by-step instructions on setting up Telemetry Viewer GUI over UDP.

---

## 🧪 Testing & Diagnostic Suite

Run test scripts from the root directory:

```bash
# 1. Run Packet Parser Unit Tests
python3 tests/test_pkt_parse.py

# 2. Run CRC16 Diagnostic Verification
python3 tests/test_crc_diag.py

# 3. Simulate 80kHz ESP32 UDP Sender (No hardware required)
python3 tests/mock_udp_sender.py --ip 127.0.0.1 --port 5005 --rate 200

# 4. Monitor UDP Traffic & Packet Rates
python3 tests/udp_diagnostic_listener.py --port 5005

# 5. ESP32 Network Troubleshooter & Ping Tool
python3 tests/esp32_network_troubleshooter.py --ip 192.168.1.150
```

---

## 🛠️ Comprehensive Troubleshooting Guide

| Symptom | Probable Cause | Solution / Fix |
| :--- | :--- | :--- |
| `[WAITING] No data received` | Host firewall blocking UDP port `5005` | Add firewall rule: `sudo ufw allow 5005/udp` |
| `[WAITING] No data received` | ESP32 Wi-Fi fail / wrong IP | Verify Wi-Fi SSID/password in `secrets.h`. Run `tests/esp32_network_troubleshooter.py` |
| `CRC Mismatch Error` | Wi-Fi packet corruption / packet truncation | Ensure router is on 2.4 GHz channel without heavy interference. Verify `EXPECTED_PAYLOAD_SIZE = 1210` |
| `High Dropped Packets` | OS UDP socket buffer overflow | `receiver.py` sets `SO_RCVBUF` to 4MB automatically. Increase system limit: `sysctl -w net.core.rmem_max=4194304` |
| `Flatline (ADC <= 5)` | Sensor disconnected / open pin | Check piezo wiring on GPIO 34. Ensure common ground between piezo circuit and ESP32 GND |
| `Saturation (ADC >= 4090)` | Voltage exceeds 3.3V rail | Add voltage divider or attenuation circuit on piezo signal line before GPIO 34 |
