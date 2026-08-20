# Telemetry Viewer Configuration & Setup Guide

Telemetry Viewer (by Farrell Farhan) is a high-performance open-source GUI for displaying real-time telemetry over UDP, Serial, or TCP.

---

## Method 1: Direct UDP Stream from MCU to Telemetry Viewer

Telemetry Viewer supports parsing binary frames directly over UDP.

### 1. Telemetry Viewer Data Structure Settings
- **Port**: `5005` (UDP)
- **Data Structure**: Binary
- **Endianness**: Little Endian (`<`)
- **Sync Byte**: `0xAA`, `0x55` (2 bytes)

### 2. Field Layout Definitions
Add the following fields in Telemetry Viewer's configuration dialog:

| Field Name | Type | Size | Notes |
| :--- | :--- | :--- | :--- |
| **Sync 1** | `uint8` | 1 byte | Value: `0xAA` |
| **Sync 2** | `uint8` | 1 byte | Value: `0x55` |
| **Sequence Number** | `uint32` | 4 bytes | Packet ID / Loss counter |
| **Sample Count** | `uint16` | 2 bytes | 512 |
| **Piezo ADC** | `int16` / `uint16` | 2 bytes | **Plotted Signal** (Select Chart: Line Graph) |
| **CRC16** | `uint16` | 2 bytes | Checksum |

---

## Method 2: CSV / Python Bridge Mode

If you are using `receiver_converter.py` to stream and log data continuously, you can stream to CSV while using Telemetry Viewer or Python Live Plotter.

### Running Receiver & CSV Converter
```bash
python3 receiver_converter.py --ip 0.0.0.0 --port 5005 --csv piezo_data.csv --verbose
```

### Generated CSV Format
```csv
ts,packet,adc
1786441091.881660,0,2048
1786441091.881672,0,2511
1786441091.881685,0,2929
```
