#!/usr/bin/env python3
"""
ESP32 ADC CSV Receiver & Logger
Listens on HTTP (or Serial) to receive incoming CSV batches from ESP32,
saves the data into a local CSV file ('adc_data.csv'),
and displays ONLY the live sample rate on the console output.
"""
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
import sys
CSV_FILENAME = "adc_data.csv"
PORT = 8080
total_samples_received = 0
last_report_time = time.time()
window_samples = 0
# Create / append to CSV file with header if missing
try:
    with open(CSV_FILENAME, "x") as f:
        f.write("timestamp_sec,ms,adc_value\n")
except FileExistsError:
    pass
class CSVHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        global total_samples_received, window_samples, last_report_time
        content_length = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_length).decode('utf-8')
        lines = post_data.strip().split('\n')
        samples_in_batch = 0
        with open(CSV_FILENAME, "a") as f:
            for line in lines:
                if line.startswith("timestamp") or not line:
                    continue
                f.write(line + "\n")
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include "esp_heap_caps.h"
…
        // Clean single-line output printing ONLY the sample rate
        Serial.printf("Sample Rate: %.1f Hz\n", rate);
        totalSamples = 0;
        lastReport = now;
    }
}
                samples_in_batch += 1
        total_samples_received += samples_in_batch
        window_samples += samples_in_batch
        # Send HTTP 200 OK
        self.send_response(200)
        self.end_headers()
        # Update & display sample rate every 1 second
        now = time.time()
        elapsed = now - last_report_time
        if elapsed >= 1.0:
            rate = window_samples / elapsed
            # Print ONLY the sample rate
            print(f"Sample Rate: {rate:.1f} Hz (Total saved: {total_samples_received})")
            window_samples = 0
            last_report_time = now
    def log_message(self, format, *args):
        # Suppress default HTTP request logging to keep console clean
        return
def run_server():
    server_address = ('', PORT)
    httpd = HTTPServer(server_address, CSVHandler)
    print(f"Server started at http://localhost:{PORT}")
    print("Saving incoming data to 'adc_data.csv' and displaying ONLY the sample rate...\n")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server.")
if __name__ == "__main__":
    run_server()
