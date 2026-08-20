# 80kHz Piezo Sensor DMA & UDP Transport Engine (v2.0)

High-performance, ultra-low-latency 80,000 Hz continuous acquisition system for piezoelectric sensors using ESP32 Hardware DMA, UDP Wi-Fi burst streaming, and Power-Optimized Deep Sleep Engine.

---

## 📖 Table of Contents
1. [Theory of Operation](#-theory-of-operation)
2. [Repository & Directory Structure](#-repository--directory-structure)
3. [Empirical Power Measurements (Nordic PPK2)](#-empirical-power-measurements-nordic-ppk2)
4. [Binary Protocol Specification](#-binary-protocol-specification)
5. [Firmware Variants (v1.0 Baseline vs v2.0 Deep Sleep)](#-firmware-variants)
6. [Commands & Execution Guide](#-commands--execution-guide)
7. [Telemetry & Web Visualizer](#-telemetry--web-visualizer)
8. [Testing & Diagnostic Suite](#-testing--diagnostic-suite)
9. [Comprehensive Troubleshooting Guide](#-comprehensive-troubleshooting-guide)

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

4. **v2.0 Power Optimization & Deep Sleep Engine**:
   - **Duty-Cycled Operation**: Samples continuously at 80 kHz for a configured active window (e.g., 60 seconds / 4.8 Million samples).
   - **Full Peripheral Shutdown**: Automatically uninstalls I2S DMA drivers, turns off the Wi-Fi radio (`WIFI_OFF`), and powers down CPU & RTC domain peripherals.
   - **Low-Power RTC Timer Wakeup**: Enters deep sleep mode for a configurable duration (e.g., 180 seconds). Boot count is preserved in RTC slow memory (`RTC_DATA_ATTR`).

---

## 🔋 Empirical Power Measurements (Nordic PPK2)

Measured empirically using a **Nordic Power Profiler Kit II (PPK2)** under real 80,000 Hz UDP streaming and deep sleep duty cycles:
<img width="1556" height="649" alt="image" src="https://[github.com/psudocoderr/piezo-dma/blob/main/assets/ppk2_power_profile.png]" />


```
       Peak Wi-Fi TX Spike: 256 mA
                 ▲
   200mA ────────┼────────────┐
                 │  Active    │ (136 mA Avg)
   100mA ────────┴────────────┘
     0mA ─────────────────────██████████████████ (4.32 mA Deep Sleep Baseline)
         └────────┬──────────┘└───────┬────────┘
             1 Min Active        3 Min Deep Sleep
             
           Overall Cycle Average: 38.21 mA
```

* **Deep Sleep Baseline**: **~4.32 mA** (ESP32 core in deep sleep ~10µA + DevKit V1 onboard CP2102/CH340 USB-UART bridge & AMS1117 LDO quiescent draw).
* **Active Operational Average**: **~136.00 mA** (Wi-Fi TX @ 200 pkts/sec + 80kHz DMA engine).
* **Maximum Peak Current**: **256.00 mA** (Wi-Fi RF transmit spikes).
* **Total Cycle Average**: **~38.21 mA** (Weighted average over 1-min active / 3-min sleep).

For full details, hardware setup diagrams, and test screenshots, see the dedicated [firmware_esp32_dma_udp_deepsleep README](file:///home/lyniks0611/Cloud/OneDrive02/Documents/Arduino/piezo-dma/firmware/firmware_esp32_dma_udp_deepsleep/README.md).

---

## 📁 Repository & Directory Structure

```
piezo-dma/
├── .gitignore                      # Git ignore rules (filters python bytecode, binaries, secrets)
├── README.md                       # Main operational & protocol manual
├── assets/                                   # Root Assets Directory
│   ├── ppk2_power_profile.png                # Nordic PPK2 measurement plot
│   ├── csv_data_logger_3v3.png               # 3.3V rail saturation CSV logger
│   └── telemetry_viewer_scope.png            # Telemetry Viewer v0.9 scope
├── firmware/
│   ├── firmware_esp32_dma_udp/               # (v1.0) Standard DMA UDP Firmware (Always On)
│   │   ├── firmware_esp32_dma_udp.ino
│   │   ├── secrets.h.example
│   │   └── secrets.h
│   └── firmware_esp32_dma_udp_deepsleep/     # (v2.0) Power-Optimized Deep Sleep Firmware
│       ├── firmware_esp32_dma_udp_deepsleep.ino
│       ├── secrets.h.example
│       ├── README.md                          # Dedicated Deep Sleep manual & PPK2 profile
│       ├── secrets.h
│       └── assets/                            # Test screenshots (PPK2, CSV log, Telemetry scope)
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

## ⚡ Firmware Variants

| Feature | `v1.0` Standard (`firmware_esp32_dma_udp`) | `v2.0` Deep Sleep (`firmware_esp32_dma_udp_deepsleep`) |
| :--- | :--- | :--- |
| **Operating Mode** | Continuous 24/7 Streaming | Duty-Cycled Burst (1 min active / 3 min sleep) |
| **Average Current** | ~136 mA – 160 mA (Active Wi-Fi) | **~38.21 mA** (Cycle Average) / **4.32 mA** (Sleep Baseline) |
| **Wakeup Mechanism** | Power-on / Hardware Reset | Low-Power RTC Timer Wakeup |
| **RTC Data Retention** | No | Yes (`RTC_DATA_ATTR bootCount`) |

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
Select your desired variant:
* **Standard 24/7 Streaming**: Use `firmware/firmware_esp32_dma_udp/`
* **Deep Sleep Duty Cycle**: Use `firmware/firmware_esp32_dma_udp_deepsleep/`

```bash
# Copy credentials template
cp firmware/firmware_esp32_dma_udp_deepsleep/secrets.h.example firmware/firmware_esp32_dma_udp_deepsleep/secrets.h
```
Edit `secrets.h` with your Wi-Fi SSID, password, and target IP address.

### 2. Main Receiver & CSV Logging Server
Run the primary receiver server on the Host PC:
```bash
python3 server/receiver.py --ip 0.0.0.0 --port 5005 --csv piezo_data.csv --verbose
```

---

## 📊 Telemetry & Web Visualizer

Start the real-time telemetry server:
```bash
# Terminal 1: Launch WebSocket Relay
python3 telemetry/telemetry_relay.py --port 5005 --ws-port 8081

# Terminal 2: Launch Web Server Dashboard
python3 telemetry/telemetry_web_server.py --port 8080
```
Navigate to `http://localhost:8080` in your web browser.

---

## 🧪 Testing & Diagnostic Suite

```bash
# 1. Run Packet Parser Unit Tests
python3 tests/test_pkt_parse.py

# 2. Run CRC16 Diagnostic Verification
python3 tests/test_crc_diag.py

# 3. Simulate 80kHz ESP32 UDP Sender
python3 tests/mock_udp_sender.py --ip 127.0.0.1 --port 5005 --rate 200

# 4. Monitor UDP Traffic & Packet Rates
python3 tests/udp_diagnostic_listener.py --port 5005
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
