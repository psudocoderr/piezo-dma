# ⚡ ESP32 80kHz DMA UDP Deep Sleep Firmware Engine

High-performance, power-optimized C++ firmware for the **ESP32 DevKit V1** (ESP32-D0WD / ESP32-WROOM-32) that performs duty-cycled 80,000 Hz continuous DMA acquisition with low-power deep sleep management.

---

## 📖 Table of Contents
1. [Overview & Key Features](#-overview--key-features)
2. [Empirical Power Measurements (Nordic PPK2)](#-empirical-power-measurements-nordic-ppk2)
3. [Hardware & Test Setup](#-hardware--test-setup)
4. [Real-Time Telemetry & Data Logging](#-real-time-telemetry--data-logging)
5. [Architecture & Duty-Cycle Timing](#-architecture--duty-cycle-timing)
6. [Configuration & Setup Guide](#-configuration--setup-guide)
7. [Shutdown & Deep Sleep Flow](#-shutdown--deep-sleep-flow)
8. [Binary Protocol Summary](#-binary-protocol-summary)
9. [Troubleshooting & Diagnostics](#-troubleshooting--diagnostics)

---

## 🔬 Overview & Key Features

This firmware is designed for battery- or solar-assisted piezoelectric monitoring nodes using the **ESP32 DevKit V1** development board.

* **Ultra-High Speed Sampling**: 80,000 Hz continuous acquisition using the ESP32 Hardware `I2S0` DMA engine (or VSPI DMA for external SPI ADCs).
* **Duty-Cycled Operation**: Collects & streams data for an **Active Window** (default: 60 seconds / ~4.8 million samples), then enters **Deep Sleep** (default: 180 seconds / 3 minutes).
* **RTC State Retention**: Utilizes ESP32 RTC slow memory (`RTC_DATA_ATTR`) to persist `bootCount` across deep sleep reset cycles.
* **Complete Hardware Teardown**: Prior to entering sleep, explicitly releases hardware DMA buffers, disables the I2S ADC driver, disconnects Wi-Fi, powers off the Wi-Fi radio, and disables RTC peripherals.
* **3-Byte Sample Framing**: Enforces `[0xAA (Sync)][MSB][LSB]` framing per reading.
* **Integrity & Sanity Checks**: Monotonic 32-bit packet sequence numbers, 16-bit CRC16-CCITT checksums, and real-time ADC rail saturation / flatline detection.

---

## 🔋 Empirical Power Measurements (Nordic PPK2)

Power consumption was measured empirically using a **Nordic Power Profiler Kit II (PPK2)** under real 80,000 Hz continuous UDP streaming and deep sleep cycles.

![Nordic PPK2 Power Profile Plot](file:///home/lyniks0611/Cloud/OneDrive02/Documents/Arduino/piezo-dma/assets/ppk2_power_profile.png)

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

### Measured Power Metrics:

| Operating State | Empirical Current | Hardware Component Activity |
| :--- | :--- | :--- |
| **Deep Sleep Baseline** | **~4.32 mA** | ESP32 Core in Deep Sleep (~10µA), onboard CP2102/CH340 USB-UART + LDO quiescent draw (~4.3 mA) |
| **Active Operational Average** | **~136.00 mA** | CPU @ 240MHz, I2S0 ADC DMA Engine, Wi-Fi TX @ 200 pkts/sec (80,000 Hz) |
| **Maximum Peak Current** | **256.00 mA** | Wi-Fi RF Power Amplifier transmit burst spikes |
| **Total Cycle Average** | **~38.21 mA** | Weighted average over 1-minute active + 3-minute sleep cycle |

> 💡 **Note on Hardware Optimization**: The ~4.32 mA deep sleep baseline current is caused by the DevKit V1 onboard USB-Serial chip and AMS1117 LDO. For ultra-low power applications requiring <15 µA deep sleep, bypass the USB bridge or use a bare ESP-WROOM-32 module with a high-efficiency switching regulator (e.g., TPS62840).

---

## 📌 Hardware & Test Setup

### Verification Test Conditions:
During validation and power profiling, **GPIO 34 (`ADC1_CHANNEL_6`)** was connected directly to **HIGH (3.3V)** to verify maximum rail-to-rail saturation tracking (`adc_raw = 4095`, `voltage_v = 3.300V`).

#### 1. Internal ADC Mode (`MODE_I2S_ADC_DMA` - Default)
| Signal / Function | ESP32 Pin | Test State / Connection |
| :--- | :--- | :--- |
| **Piezo Analog Input** | **GPIO 34** (`ADC1_CHANNEL_6`) | Tied to 3.3V (Rail Saturation Test) |
| **Common Ground** | **GND** | Sensor / Power Supply Common Ground |

#### 2. External SPI ADC Mode (`MODE_SPI_DMA` - Optional)
| Signal / Function | ESP32 Pin | Notes |
| :--- | :--- | :--- |
| **VSPI MISO** | **GPIO 19** | Master In Slave Out |
| **VSPI MOSI** | **GPIO 23** | Master Out Slave In |
| **VSPI SCK** | **GPIO 18** | SPI Serial Clock |
| **VSPI CS** | **GPIO 5** | Chip Select |

---

## 📊 Real-Time Telemetry & Data Logging

### 1. CSV Output Validation (3.3V Saturation Test)
The high-speed UDP receiver logs incoming datagrams with microsecond precision timestamps, sequence IDs, 12-bit raw ADC values, and converted voltage readings:

![3.3V Rail CSV Data Logger Output](file:///home/lyniks0611/Cloud/OneDrive02/Documents/Arduino/piezo-dma/assets/csv_data_logger_3v3.png)

```csv
ts,packet,adc_raw,voltage_v
17-08-2026 14:06:35:855.855300,84402,4095,3.300
17-08-2026 14:06:35:855.855313,84402,4095,3.300
17-08-2026 14:06:35:855.855325,84402,4095,3.300
```

### 2. Real-Time Telemetry Viewer Scope
Binary sample frames (`SYNC 0xAA`, `msb`, `lsb`) visualized live using Telemetry Viewer over UDP port 5005:

![Telemetry Viewer v0.9 Scope](file:///home/lyniks0611/Cloud/OneDrive02/Documents/Arduino/piezo-dma/assets/telemetry_viewer_scope.png)

---

## ⏱️ Architecture & Duty-Cycle Timing

```
                       ACTIVE WINDOW (60 sec)
   ┌─────────────────────────────────────────────────────────────┐
   │ • Connect Wi-Fi & Init I2S DMA Engine                        │
   │ • Sample 80,000 Hz -> Transmit 1,210-byte UDP Datagrams     │  DEEP SLEEP WINDOW (180 sec)
   │ • Stream 200 packets/sec (4.8 Million samples total)         │ ┌──────────────────────────┐
   └──────────────────────────────┬──────────────────────────────┘ │ • Wi-Fi Radio OFF        │
                                  │                                │ • Main CPU & DMA OFF     │
                                  ▼                                │ • Baseline: ~4.32 mA     │
                     [ Hardware Teardown Routine ]                 └────────────┬─────────────┘
                     • Uninstall I2S Driver                                     │
                     • Power off Wi-Fi Radio                                    │
                     • Set RTC Timer Wakeup (180s)                              │
                     • esp_deep_sleep_start() ──────────────────────────────┘
```

### Configurable Timing Parameters (`firmware_esp32_dma_udp_deepsleep.ino`):
```cpp
#define ACTIVE_DURATION_MS 60000ULL   // Data collection duration (60,000 ms = 1 minute)
#define TIME_TO_SLEEP_SEC  180         // Deep sleep duration (180 s = 3 minutes)
```

---

## ⚙️ Configuration & Setup Guide

### 1. Configure Secrets & Network Settings
1. Copy `secrets.h.example` to `secrets.h`:
   ```bash
   cp secrets.h.example secrets.h
   ```
2. Edit `secrets.h` with your Wi-Fi network and host server IP:
   ```cpp
   #ifndef SECRETS_H
   #define SECRETS_H

   #define WIFI_SSID     "Your_WiFi_SSID"
   #define WIFI_PASSWORD "Your_WiFi_Password"
   #define UDP_DEST_IP   "192.168.1.100"   // IP address of Host PC running receiver.py
   #define UDP_DEST_PORT 5005

   #endif
   ```

### 2. Flashing via Arduino IDE
1. Board Selection: **ESP32 Dev Module**
2. CPU Frequency: **240 MHz (WiFi/BT)**
3. Flash Frequency: **80 MHz**
4. Upload Speed: **921600**
5. Open Serial Monitor at **3,000,000 baud** (or **115,200 baud** to read boot log headers).

---

## 🛑 Shutdown & Deep Sleep Flow

When `ACTIVE_DURATION_MS` (60 seconds) expires, `prepare_and_enter_deep_sleep()` executes in 5 sequential steps:

```cpp
void prepare_and_enter_deep_sleep() {
    // 1. Release DMA and I2S hardware drivers
    i2s_adc_disable(I2S_NUM);
    i2s_driver_uninstall(I2S_NUM);

    // 2. Shut down Wi-Fi radio completely
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // 3. Disable RTC domain peripherals for maximum power reduction
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);

    // 4. Configure RTC timer wakeup source
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_SEC * uS_TO_S_FACTOR);

    // 5. Enter Deep Sleep
    esp_deep_sleep_start();
}
```

---

## 📦 Binary Protocol Summary

### Total Datagram Size: 1,210 Bytes (200 Datagrams / Second)

* **Header (8 Bytes)**:
  - `0x00`: `magic1` (`0xAA`)
  - `0x01`: `magic2` (`0x55`)
  - `0x02`: `sequence_num` (`uint32_t`, continuous counter across bursts)
  - `0x06`: `sample_count` (`uint16_t` = 400)
* **Payload (1,200 Bytes)**: 400 3-byte readings `[0xAA][ADC_MSB][ADC_LSB]`.
* **Footer (2 Bytes)**: `CRC16-CCITT` (`uint16_t`).

---

## 🛠️ Troubleshooting & Diagnostics

| Symptom | Cause | Solution |
| :--- | :--- | :--- |
| **Boot loop on wakeup / reset** | Battery brownout during 256 mA Wi-Fi TX peak | Add a 470 µF low-ESR capacitor across ESP32 `3V3` and `GND` pins |
| **Baseline sleep current > 4.3mA** | Onboard USB-to-UART bridge / LDO | Normal for DevKit V1 board. Cut power LED or use bare ESP-WROOM-32 module |
| **No UDP packets received after 1st cycle** | Host server stopped or IP changed | Ensure `receiver.py` is running continuously on the host PC |
| **Garbled Serial Monitor output** | Baud rate mismatch | Set Serial Monitor speed to **3,000,000 baud** |
