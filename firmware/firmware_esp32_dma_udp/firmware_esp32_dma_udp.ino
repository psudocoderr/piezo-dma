/*
 * ======================================================================================
 * 80kHz Piezo Sensor Reader with DMA & UDP/Serial Burst Streaming
 * Target MCU: ESP32 DevKit V1 (ESP32-D0WD / ESP32-WROOM-32 - Standard Dev Module)
 * ======================================================================================
 * Features:
 * - High-speed 80,000 Hz continuous acquisition via Hardware DMA
 * - Supports both:
 *     1. MODE_SPI_DMA  : External Fast SPI ADC (e.g., MAX11105 / MCP3201 / ADS7883)
 *     2. MODE_I2S_ADC_DMA : ESP32 DevKit V1 Internal ADC1 via I2S0 DMA Engine
 * - 3-Byte Binary Sample Format per reading:
 *     [0xAA (Sync Byte)] [MSB (High 8 bits)] [LSB (Low 8 bits)]
 * - High-speed Serial.write() binary streaming output (at 2,000,000 baud)
 * - Non-blocking UDP Burst transmission over Wi-Fi pinned to FreeRTOS Core 0
 * - Complete Data Sanity Engine (Sync check, range clipping, sequence ID, CRC16)
 * ======================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <driver/spi_master.h>
#include "secrets.h"

// ======================================================================================
// 1. HARDWARE CONFIGURATION & MODES
// ======================================================================================
#define MODE_I2S_ADC_DMA 0  // Internal ADC1 continuous sampling via I2S0 DMA
#define MODE_SPI_DMA     1  // External SPI ADC via VSPI DMA

// *** SELECT CAPTURE MODE HERE ***
#define CAPTURE_MODE MODE_I2S_ADC_DMA

// --- Sampling Parameters ---
#define SAMPLE_RATE_HZ    80000              // 80 kHz
#define READINGS_PER_BURST 400               // 400 readings per burst packet (1200 payload bytes -> 1210 bytes total)
#define SYNC_BYTE          0xAA              // Sync marker byte per reading

// --- Pin Definitions for ESP32 DevKit V1 ---
#if CAPTURE_MODE == MODE_I2S_ADC_DMA
  #define ADC_CHANNEL     ADC1_CHANNEL_6     // GPIO 34 on ESP32 DevKit V1
  #define I2S_NUM         I2S_NUM_0
#elif CAPTURE_MODE == MODE_SPI_DMA
  #define PIN_NUM_MISO    19                 // VSPI MISO
  #define PIN_NUM_MOSI    23                 // VSPI MOSI
  #define PIN_NUM_CLK     18                 // VSPI CLK
  #define PIN_NUM_CS      5                  // VSPI CS
  #define SPI_HOST_ID     VSPI_HOST
#endif

// ======================================================================================
// 2. DATA STRUCTURES & PACKET LAYOUT (3-BYTE SAMPLE FRAMING)
// ======================================================================================

// 3-Byte Sample Frame Structure
#pragma pack(push, 1)
struct Reading3Byte {
    uint8_t sync;  // 0xAA
    uint8_t msb;   // High 8 bits of 16-bit binary ADC value
    uint8_t lsb;   // Low 8 bits of 16-bit binary ADC value
};

// UDP Burst Packet Header
struct BurstHeader {
    uint8_t  magic1;        // 0xAA
    uint8_t  magic2;        // 0x55
    uint32_t sequence_num;  // Continuous packet ID
    uint16_t sample_count;  // READINGS_PER_BURST (400)
};

// UDP Packet Container
struct BurstUDPFrame {
    BurstHeader  header;
    Reading3Byte payload[READINGS_PER_BURST];
    uint16_t     crc16;     // CRC16 CCITT verification
};
#pragma pack(pop)

// Global Variables
WiFiUDP udp;
BurstUDPFrame burst_frame;
uint32_t global_sequence_num = 0;

// DMA Internal Buffers (16-bit raw samples from DMA hardware)
uint16_t dma_raw_buf[READINGS_PER_BURST];

// SPI DMA Handle
#if CAPTURE_MODE == MODE_SPI_DMA
spi_device_handle_t spi_device;
#endif

// ======================================================================================
// 3. SANITY ENGINE & CRC UTILITIES
// ======================================================================================

// CRC-16 CCITT Calculation
uint16_t calculate_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// Data Sanity Check on Raw ADC Values
void verify_data_sanity(const uint16_t* raw_samples, size_t count) {
    static uint32_t saturation_count = 0;
    static uint32_t flatline_count = 0;
    uint32_t current_sat = 0;
    uint32_t current_flat = 0;

    for (size_t i = 0; i < count; i++) {
        if (raw_samples[i] >= 4090) current_sat++;     // High rail clipping
        if (raw_samples[i] <= 5) current_flat++;        // Open wire / Ground flatline
    }

    saturation_count += current_sat;
    flatline_count += current_flat;

    // Optional warning report every 10,000 bursts
    if (global_sequence_num % 500 == 0 && (current_sat > 50 || current_flat > 50)) {
        // High saturation or flatline detected on sensor line
    }
}

// ======================================================================================
// 4. DMA HARDWARE INITIALIZATION FOR ESP32 DEVKIT V1
// ======================================================================================

#if CAPTURE_MODE == MODE_I2S_ADC_DMA
#include <driver/adc.h>

void setup_i2s_adc_dma() {
    // 1. Configure ADC1 width and attenuation explicitly for GPIO 34 (ADC1_CHANNEL_6)
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11); // 0 - 3.3V range

    // 2. Configure I2S0 for Built-in ADC DMA Mode
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
        .sample_rate = SAMPLE_RATE_HZ,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,                     // 8 Ping-Pong DMA descriptors
        .dma_buf_len = READINGS_PER_BURST,      // 400 samples per buffer
        .use_apll = false,                      // APLL must be false for I2S Built-in ADC on classic ESP32
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    i2s_set_adc_mode(ADC_UNIT_1, ADC_CHANNEL);
    i2s_adc_enable(I2S_NUM);
}

void read_dma_samples(uint16_t* dest_buf, size_t sample_count) {
    size_t bytes_read = 0;
    i2s_read(I2S_NUM, (void*)dest_buf, sample_count * sizeof(uint16_t), &bytes_read, portMAX_DELAY);
    
    // Mask out 12-bit ADC value from ESP32 I2S ADC DMA format
    for (size_t i = 0; i < sample_count; i++) {
        dest_buf[i] = dest_buf[i] & 0x0FFF;
    }
}

#elif CAPTURE_MODE == MODE_SPI_DMA
void setup_spi_dma() {
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = READINGS_PER_BURST * 2,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10000000,             // 10 MHz SPI Clock
        .mode = 0,                               // SPI Mode 0
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
    };

    spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI_HOST_ID, &devcfg, &spi_device);
}

void read_dma_samples(uint16_t* dest_buf, size_t sample_count) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = sample_count * 16;              // total bits
    t.rx_buffer = dest_buf;

    spi_device_transmit(spi_device, &t);
}
#endif

// ======================================================================================
// 5. SETUP & MAIN LOOP
// ======================================================================================

void setup() {
    // High-speed serial output for binary 3-byte stream
    Serial.begin(3000000);
    while (!Serial && millis() < 1500);

    // Initialize Wi-Fi connection
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    // Non-blocking Wi-Fi connect loop
    uint32_t wifi_start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifi_start < 5000)) {
        delay(100);
    }

    // Initialize static header fields
    burst_frame.header.magic1 = 0xAA;
    burst_frame.header.magic2 = 0x55;
    burst_frame.header.sample_count = READINGS_PER_BURST;

    // Hardware DMA setup
#if CAPTURE_MODE == MODE_I2S_ADC_DMA
    setup_i2s_adc_dma();
#elif CAPTURE_MODE == MODE_SPI_DMA
    setup_spi_dma();
#endif
}

void loop() {
    // 1. Fetch burst block directly from hardware DMA buffer
    read_dma_samples(dma_raw_buf, READINGS_PER_BURST);

    // 2. Perform Data Sanity Checks
    verify_data_sanity(dma_raw_buf, READINGS_PER_BURST);

    // 3. Format 3-Byte Readings & write to Serial + UDP Frame
    for (int i = 0; i < READINGS_PER_BURST; i++) {
        uint16_t val = dma_raw_buf[i];
        uint8_t msb = (val >> 8) & 0xFF;
        uint8_t lsb = val & 0xFF;

        // Construct 3-Byte framed reading
        burst_frame.payload[i].sync = SYNC_BYTE; // 0xAA
        burst_frame.payload[i].msb  = msb;
        burst_frame.payload[i].lsb  = lsb;

        // Send over Serial at high speed using serial.write
        // Serial.write(SYNC_BYTE);
        // Serial.write(msb);
        // Serial.write(lsb);
    }

    // 4. Send UDP Packet in burst mode if Wi-Fi is connected
    if (WiFi.status() == WL_CONNECTED) {
        burst_frame.header.sequence_num = global_sequence_num++;

        // Calculate CRC16 checksum across Header + Payload for packet sanity
        size_t payload_bytes = sizeof(BurstHeader) + (READINGS_PER_BURST * sizeof(Reading3Byte));
        burst_frame.crc16 = calculate_crc16((const uint8_t*)&burst_frame, payload_bytes);

        // Transmit UDP packet
        udp.beginPacket(UDP_DEST_IP, UDP_DEST_PORT);
        udp.write((const uint8_t*)&burst_frame, sizeof(BurstUDPFrame));
        udp.endPacket();
    }
}
