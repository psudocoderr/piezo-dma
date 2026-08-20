#!/usr/bin/env python3
"""
======================================================================================
80kHz Piezo Live Web Telemetry Tracker & CSV Recorder Server
Target MCU: ESP32 DevKit V1 (ESP32-D0WD / ESP32-WROOM-32)
======================================================================================
Features:
- Live 80kHz UDP Packet Receiver & CSV Logger
- Built-in Zero-Dependency Web Telemetry Server (Port 8080)
- Real-time WebGL/Canvas Oscilloscope Waveform & Signal Statistics
- Works out of the box in Chrome / Firefox / Safari
======================================================================================
"""

import socket
import struct
import sys
import time
import argparse
import threading
import json
from collections import deque
from http.server import HTTPServer, ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path

DEFAULT_UDP_PORT = 5005
DEFAULT_WEB_PORT = 8080
DEFAULT_CSV_PATH = "piezo_data_80khz.csv"
SAMPLE_RATE_HZ = 80000.0
READINGS_PER_BURST = 400
SYNC_BYTE = 0xAA
EXPECTED_PAYLOAD_SIZE = 8 + (READINGS_PER_BURST * 3) + 2

# CRC16 Lookup Table
CRC16_TABLE = []
for _i in range(256):
    _curr = _i << 8
    for _ in range(8):
        if _curr & 0x8000:
            _curr = ((_curr << 1) ^ 0x1021) & 0xFFFF
        else:
            _curr = (_curr << 1) & 0xFFFF
    CRC16_TABLE.append(_curr)

def calculate_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) ^ CRC16_TABLE[((crc >> 8) ^ byte) & 0xFF]) & 0xFFFF
    return crc

# Shared Global State
ring_buffer = deque(maxlen=2000)  # Stores recent voltage samples for live web display
lock = threading.Lock()

telemetry_stats = {
    "rate_hz": 0.0,
    "total_samples": 0,
    "packets": 0,
    "drops": 0,
    "v_min": 0.0,
    "v_max": 0.0,
    "v_pp": 0.0,
    "is_connected": False,
    "last_packet_time": 0
}

HTML_CONTENT = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>80kHz Piezo Telemetry Tracker</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;700&family=JetBrains+Mono:wght@400;600&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-color: #0b0f19;
            --card-bg: rgba(22, 30, 46, 0.75);
            --border-color: rgba(255, 255, 255, 0.08);
            --accent-cyan: #00f2fe;
            --accent-blue: #4facfe;
            --accent-green: #10b981;
            --accent-red: #ef4444;
            --text-primary: #f3f4f6;
            --text-secondary: #9ca3af;
        }

        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Inter', sans-serif;
            background: var(--bg-color);
            color: var(--text-primary);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            background-image: 
                radial-gradient(circle at 15% 20%, rgba(0, 242, 254, 0.05) 0%, transparent 40%),
                radial-gradient(circle at 85% 80%, rgba(79, 172, 254, 0.05) 0%, transparent 40%);
        }

        header {
            padding: 1.25rem 2rem;
            border-bottom: 1px solid var(--border-color);
            display: flex;
            justify-content: space-between;
            align-items: center;
            background: rgba(11, 15, 25, 0.8);
            backdrop-filter: blur(12px);
        }

        .logo-group { display: flex; align-items: center; gap: 0.75rem; }
        .logo-badge {
            background: linear-gradient(135deg, var(--accent-cyan), var(--accent-blue));
            color: #000;
            font-weight: 700;
            font-size: 0.75rem;
            padding: 0.2rem 0.6rem;
            border-radius: 6px;
            letter-spacing: 0.5px;
        }
        h1 { font-size: 1.25rem; font-weight: 600; letter-spacing: -0.02em; }

        .status-pill {
            display: flex;
            align-items: center;
            gap: 0.5rem;
            padding: 0.4rem 0.8rem;
            background: rgba(255, 255, 255, 0.04);
            border: 1px solid var(--border-color);
            border-radius: 20px;
            font-size: 0.85rem;
        }
        .status-dot {
            width: 8px; height: 8px; border-radius: 50%;
            background: var(--accent-red);
            box-shadow: 0 0 10px var(--accent-red);
            transition: all 0.3s ease;
        }
        .status-dot.active {
            background: var(--accent-green);
            box-shadow: 0 0 12px var(--accent-green);
        }

        main {
            flex: 1;
            padding: 1.5rem 2rem;
            max-width: 1400px;
            width: 100%;
            margin: 0 auto;
            display: flex;
            flex-direction: column;
            gap: 1.5rem;
        }

        .metrics-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 1rem;
        }

        .card {
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 12px;
            padding: 1.25rem;
            backdrop-filter: blur(16px);
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2);
            transition: border-color 0.3s ease;
        }
        .card:hover { border-color: rgba(255, 255, 255, 0.15); }

        .card-label {
            font-size: 0.75rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            color: var(--text-secondary);
            margin-bottom: 0.5rem;
        }
        .card-value {
            font-family: 'JetBrains Mono', monospace;
            font-size: 1.6rem;
            font-weight: 600;
            color: #fff;
        }
        .card-sub { font-size: 0.75rem; color: var(--text-secondary); margin-top: 0.25rem; }

        .chart-container {
            position: relative;
            height: 450px;
            display: flex;
            flex-direction: column;
        }
        .chart-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 1rem;
        }
        .chart-title { font-size: 1rem; font-weight: 600; }

        canvas {
            width: 100%;
            height: 100%;
            border-radius: 8px;
            background: #060911;
        }

        .controls {
            display: flex;
            gap: 1rem;
            align-items: center;
            justify-content: flex-end;
        }

        button {
            background: linear-gradient(135deg, var(--accent-cyan), var(--accent-blue));
            color: #050b14;
            border: none;
            padding: 0.6rem 1.25rem;
            font-weight: 600;
            font-size: 0.85rem;
            border-radius: 8px;
            cursor: pointer;
            transition: opacity 0.2s ease;
        }
        button:hover { opacity: 0.9; }

        footer {
            padding: 1rem 2rem;
            border-top: 1px solid var(--border-color);
            font-size: 0.8rem;
            color: var(--text-secondary);
            display: flex;
            justify-content: space-between;
        }
    </style>
</head>
<body>
    <header>
        <div class="logo-group">
            <span class="logo-badge">80 kHz DMA</span>
            <h1>Piezo Telemetry Tracker</h1>
        </div>
        <div class="status-pill">
            <div id="statusDot" class="status-dot"></div>
            <span id="statusText">Disconnected</span>
        </div>
    </header>

    <main>
        <div class="metrics-grid">
            <div class="card">
                <div class="card-label">Sample Rate</div>
                <div id="valRate" class="card-value">0 Hz</div>
                <div class="card-sub">Target: 80,000 Hz</div>
            </div>
            <div class="card">
                <div class="card-label">Peak-to-Peak (Vpp)</div>
                <div id="valVpp" class="card-value">0.00 V</div>
                <div id="valMinMax" class="card-sub">Min: 0.0V | Max: 0.0V</div>
            </div>
            <div class="card">
                <div class="card-label">Total Samples Logged</div>
                <div id="valSamples" class="card-value">0</div>
                <div class="card-sub">Streamed to CSV</div>
            </div>
            <div class="card">
                <div class="card-label">Packets / Drops</div>
                <div id="valPackets" class="card-value">0 / 0</div>
                <div class="card-sub">Sequence Check</div>
            </div>
        </div>

        <div class="card chart-container">
            <div class="chart-header">
                <div class="chart-title">Real-Time Piezo Oscilloscope (Voltage Signal)</div>
                <div class="controls">
                    <span style="font-size: 0.8rem; color: var(--text-secondary);">Scale: 0V - 3.3V</span>
                </div>
            </div>
            <canvas id="scopeCanvas"></canvas>
        </div>
    </main>

    <footer>
        <span>Target MCU: ESP32 DevKit V1 (I2S0 / VSPI DMA)</span>
        <span>CSV Storage: piezo_data_80khz.csv</span>
    </footer>

    <script>
        const canvas = document.getElementById('scopeCanvas');
        const ctx = canvas.getContext('2d');

        function resizeCanvas() {
            canvas.width = canvas.clientWidth * window.devicePixelRatio;
            canvas.height = canvas.clientHeight * window.devicePixelRatio;
        }
        window.addEventListener('resize', resizeCanvas);
        resizeCanvas();

        let dataPoints = [];
        const maxPoints = 1000;

        function drawOscilloscope() {
            const width = canvas.width;
            const height = canvas.height;

            ctx.fillStyle = '#060911';
            ctx.fillRect(0, 0, width, height);

            // Draw Grid Lines
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
            ctx.lineWidth = 1;
            for (let y = 0; y <= height; y += height / 6) {
                ctx.beginPath();
                ctx.moveTo(0, y);
                ctx.lineTo(width, y);
                ctx.stroke();
            }
            for (let x = 0; x <= width; x += width / 10) {
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, height);
                ctx.stroke();
            }

            // Center reference line (1.65V)
            ctx.strokeStyle = 'rgba(79, 172, 254, 0.2)';
            ctx.beginPath();
            ctx.moveTo(0, height / 2);
            ctx.lineTo(width, height / 2);
            ctx.stroke();

            if (dataPoints.length < 2) return;

            // Draw Waveform
            ctx.beginPath();
            ctx.strokeStyle = '#00f2fe';
            ctx.lineWidth = 2 * window.devicePixelRatio;

            const sliceWidth = width / (dataPoints.length - 1);
            let x = 0;

            for (let i = 0; i < dataPoints.length; i++) {
                // Map 0V - 3.3V to Canvas Y
                const voltage = dataPoints[i];
                const y = height - ((voltage / 3.3) * height);

                if (i === 0) {
                    ctx.moveTo(x, y);
                } else {
                    ctx.lineTo(x, y);
                }
                x += sliceWidth;
            }
            ctx.stroke();
        }

        // Connect to SSE Stream
        const evtSource = new EventSource("/stream");

        evtSource.onmessage = function(e) {
            const data = JSON.parse(e.data);

            // Update Status Pill
            const statusDot = document.getElementById('statusDot');
            const statusText = document.getElementById('statusText');
            if (data.is_connected) {
                statusDot.classList.add('active');
                statusText.innerText = 'Streaming (80 kHz)';
            } else {
                statusDot.classList.remove('active');
                statusText.innerText = 'Waiting for ESP32...';
            }

            // Update Numerical Cards
            document.getElementById('valRate').innerText = data.rate_hz.toLocaleString() + ' Hz';
            document.getElementById('valVpp').innerText = data.v_pp.toFixed(3) + ' V';
            document.getElementById('valMinMax').innerText = `Min: ${data.v_min.toFixed(2)}V | Max: ${data.v_max.toFixed(2)}V`;
            document.getElementById('valSamples').innerText = data.total_samples.toLocaleString();
            document.getElementById('valPackets').innerText = `${data.packets.toLocaleString()} / ${data.drops}`;

            // Append waveform batch
            if (data.samples && data.samples.length > 0) {
                dataPoints = data.samples;
            }

            drawOscilloscope();
        };
    </script>
</body>
</html>
"""

class WebServerHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.end_headers()
            self.wfile.write(HTML_CONTENT.encode('utf-8'))

        elif self.path == '/stream':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'keep-alive')
            self.end_headers()

            try:
                while True:
                    with lock:
                        # Downsample buffer for web streaming (take 400 points)
                        sample_slice = list(ring_buffer)
                        step = max(1, len(sample_slice) // 400)
                        downsampled = [sample_slice[i] for i in range(0, len(sample_slice), step)]

                        payload = {
                            "is_connected": telemetry_stats["is_connected"],
                            "rate_hz": round(telemetry_stats["rate_hz"], 1),
                            "total_samples": telemetry_stats["total_samples"],
                            "packets": telemetry_stats["packets"],
                            "drops": telemetry_stats["drops"],
                            "v_min": round(telemetry_stats["v_min"], 3),
                            "v_max": round(telemetry_stats["v_max"], 3),
                            "v_pp": round(telemetry_stats["v_pp"], 3),
                            "samples": downsampled
                        }

                    msg = f"data: {json.dumps(payload)}\n\n"
                    self.wfile.write(msg.encode('utf-8'))
                    self.wfile.flush()
                    time.sleep(0.05)  # 20 FPS Web Refresh

            except Exception:
                pass
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        return  # Suppress HTTP access logging

def udp_receiver_thread(udp_port: int, csv_path: str):
    csv_file_path = Path(csv_path)
    csv_file_path.parent.mkdir(parents=True, exist_ok=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", udp_port))
    sock.settimeout(1.0)

    print(f"[UDP] Listening on 0.0.0.0:{udp_port}...")
    print(f"[CSV] Output file: {csv_file_path.resolve()}")

    expected_seq = None
    start_time = None
    last_report_time = time.time()

    with open(csv_file_path, mode="w", buffering=65536, encoding="utf-8") as csv_file:
        csv_file.write("ts,adc_raw\n")

        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                if time.time() - telemetry_stats["last_packet_time"] > 2.0:
                    with lock:
                        telemetry_stats["is_connected"] = False
                continue

            if len(data) != EXPECTED_PAYLOAD_SIZE:
                continue

            magic1, magic2, seq, count = struct.unpack("<BBIH", data[0:8])
            if magic1 != 0xAA or magic2 != 0x55:
                continue

            crc_received = struct.unpack("<H", data[-2:])[0]
            crc_calc = calculate_crc16(data[:-2])
            if crc_received != crc_calc:
                continue

            recv_time = time.time()
            if start_time is None:
                start_time = recv_time

            # Sequence tracking
            dropped = 0
            if expected_seq is not None and seq != expected_seq:
                dropped = (seq - expected_seq) & 0xFFFFFFFF
            expected_seq = (seq + 1) & 0xFFFFFFFF

            payload = data[8:8 + (count * 3)]
            voltages = []
            csv_lines = []
            base_ts = recv_time - (count * (1.0 / SAMPLE_RATE_HZ))

            for i in range(count):
                msb = payload[i * 3 + 1]
                lsb = payload[i * 3 + 2]
                adc_val = (msb << 8) | lsb
                v = (adc_val / 4095.0) * 3.3
                voltages.append(v)

                sample_ts = base_ts + (i * (1.0 / SAMPLE_RATE_HZ))
                csv_lines.append(f"{sample_ts:.6f},{adc_val}\n")

            csv_file.writelines(csv_lines)

            # Update shared telemetry metrics
            with lock:
                telemetry_stats["is_connected"] = True
                telemetry_stats["last_packet_time"] = recv_time
                telemetry_stats["packets"] += 1
                telemetry_stats["drops"] += dropped
                telemetry_stats["total_samples"] += count
                ring_buffer.extend(voltages)

                if voltages:
                    v_min = min(voltages)
                    v_max = max(voltages)
                    telemetry_stats["v_min"] = v_min
                    telemetry_stats["v_max"] = v_max
                    telemetry_stats["v_pp"] = v_max - v_min

            now = time.time()
            if now - last_report_time >= 1.0:
                elapsed = now - start_time
                with lock:
                    telemetry_stats["rate_hz"] = telemetry_stats["total_samples"] / elapsed if elapsed > 0 else 0
                last_report_time = now

def main():
    parser = argparse.ArgumentParser(description="80kHz Piezo Web Telemetry Server")
    parser.add_argument("--udp-port", type=int, default=DEFAULT_UDP_PORT, help="UDP listen port (default: 5005)")
    parser.add_argument("--web-port", type=int, default=DEFAULT_WEB_PORT, help="Web server port (default: 8080)")
    parser.add_argument("--csv", default=DEFAULT_CSV_PATH, help="Output CSV filepath")
    args = parser.parse_args()

    # Start UDP receiver in background thread
    t = threading.Thread(target=udp_receiver_thread, args=(args.udp_port, args.csv), daemon=True)
    t.start()

    # Start HTTP Web Server
    server_address = ('', args.web_port)
    httpd = ThreadingHTTPServer(server_address, WebServerHandler)
    print(f"\n==========================================================")
    print(f"  🚀 80kHz Piezo Telemetry Tracker Active!")
    print(f"  👉 Open in browser: http://localhost:{args.web_port}")
    print(f"==========================================================\n")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[EXIT] Server stopped by user.")

if __name__ == "__main__":
    main()
