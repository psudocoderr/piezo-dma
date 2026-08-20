/*
 * ======================================================================================
 * 80kHz Piezo Reader (DMA + UDP/Serial Burst) with 1-Minute Timer Deep Sleep Engine
 * Target MCU: ESP32 DevKit V1 (ESP32-D0WD / ESP32-WROOM-32)
 * ======================================================================================
 * Features:
 * - High-speed 80,000 Hz acquisition via Hardware DMA for 1 Minute (60 seconds).
 * - Shuts down Wi-Fi, CPU, DMA Engine, and RTC Peripherals after 60s.
 * - Powers down EVERYTHING except the low-power RTC Timer.
 * - Wakeup Source: RTC Timer ONLY (wakes up after configured sleep duration).
 * - Retains all 3-Byte Sample Framing [0xAA][MSB][LSB], UDP Burst, Serial, CRC16.
 * ======================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <driver/spi_master.h>
#include <driver/adc.h>
#include <driver/rtc_io.h>
#include "esp_sleep.h"
#include "soc/rtc_periph.h"
#include "secrets.h"

// ======================================================================================
// 1. SLEEP & TIMING CONFIGURATION
// ======================================================================================
#define ACTIVE_DURATION_MS 60000ULL         // Collect data for 1 Minute (60,000 ms)
#define TIME_TO_SLEEP_SEC  180               // Deep sleep timer duration (in seconds)
#define uS_TO_S_FACTOR     1000000ULL       // Microseconds to seconds conversion factor

RTC_DATA_ATTR int bootCount = 0;           // Preserved across Deep Sleep reboots in RTC slow memory
uint32_t active_start_time = 0;            // Tracks active runtime duration

// ======================================================================================
// 2. HARDWARE CONFIGURATION & MODES
// ======================================================================================
#define MODE_I2S_ADC_DMA 0  // Internal ADC1 continuous sampling via I2S0 DMA
#define MODE_SPI_DMA     1  // External SPI ADC via VSPI DMA

// *** SELECT CAPTURE MODE HERE ***
#define CAPTURE_MODE MODE_I2S_ADC_DMA

// --- Sampling Parameters ---
#define SAMPLE_RATE_HZ    80000             // 80 kHz
#define READINGS_PER_BURST 400              // 400 readings per burst packet (1200 payload bytes)
#define SYNC_BYTE          0xAA             // Sync marker byte per reading

// --- Pin Definitions for ESP32 DevKit V1 ---
#if CAPTURE_MODE == MODE_I2S_ADC_DMA
  #define ADC_CHANNEL     ADC1_CHANNEL_6    // GPIO 34 on ESP32 DevKit V1
  #define I2S_NUM         I2S_NUM_0
#elif CAPTURE_MODE == MODE_SPI_DMA
  #define PIN_NUM_MISO    19                // VSPI MISO
  #define PIN_NUM_MOSI    23                // VSPI MOSI
  #define PIN_NUM_CLK     18                // VSPI CLK
  #define PIN_NUM_CS      5                 // VSPI CS
  #define SPI_HOST_ID     VSPI_HOST
#endif

// ======================================================================================
// 3. DATA STRUCTURES & PACKET LAYOUT (3-BYTE SAMPLE FRAMING)
// ======================================================================================

#pragma pack(push, 1)
struct Reading3Byte {
    uint8_t sync;  // 0xAA
    uint8_t msb;   // High 8 bits of 16-bit binary ADC value
    uint8_t lsb;   // Low 8 bits of 16-bit binary ADC value
};

struct BurstHeader {
    uint8_t  magic1;        // 0xAA
    uint8_t  magic2;        // 0x55
    uint32_t sequence_num;  // Continuous packet ID
    uint16_t sample_count;  // READINGS_PER_BURST (400)
};

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

#if CAPTURE_MODE == MODE_SPI_DMA
spi_device_handle_t spi_device;
#endif

// ======================================================================================
// 4. HELPER & SANITY UTILITIES
// ======================================================================================

void print_wakeup_reason() {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    switch(wakeup_reason) {
        case ESP_SLEEP_WAKEUP_EXT0     : Serial.println("[WAKEUP] Ext signal (RTC_IO)"); break;
        case ESP_SLEEP_WAKEUP_EXT1     : Serial.println("[WAKEUP] Ext signal (RTC_CNTL)"); break;
        case ESP_SLEEP_WAKEUP_TIMER    : Serial.println("[WAKEUP] RTC Timer"); break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("[WAKEUP] Touchpad"); break;
        default                        : Serial.printf("[WAKEUP] Power-on / Reset (Cause Code: %d)\n", wakeup_reason); break;
    }
}

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

void verify_data_sanity(const uint16_t* raw_samples, size_t count) {
    static uint32_t saturation_count = 0;
    static uint32_t flatline_count = 0;
    uint32_t current_sat = 0;
    uint32_t current_flat = 0;

    for (size_t i = 0; i < count; i++) {
        if (raw_samples[i] >= 4090) current_sat++;     // Rail saturation
        if (raw_samples[i] <= 5) current_flat++;        // Ground flatline
    }

    saturation_count += current_sat;
    flatline_count += current_flat;
}

// ======================================================================================
// 5. DMA HARDWARE INITIALIZATION
// ======================================================================================

#if CAPTURE_MODE == MODE_I2S_ADC_DMA

void setup_i2s_adc_dma() {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11); // 0 - 3.3V range

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
        .sample_rate = SAMPLE_RATE_HZ,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,                     
        .dma_buf_len = READINGS_PER_BURST,      
        .use_apll = false,                      
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
        .clock_speed_hz = 10000000,             
        .mode = 0,                               
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 7,
    };

    spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI_HOST_ID, &devcfg, &spi_device);
}

void read_dma_samples(uint16_t* dest_buf, size_t sample_count) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = sample_count * 16;
    t.rx_buffer = dest_buf;

    spi_device_transmit(spi_device, &t);
}
#endif

// ======================================================================================
// 6. SHUTDOWN & DEEP SLEEP PREPARATION ENGINE
// ======================================================================================

void prepare_and_enter_deep_sleep() {
    Serial.println("\n[SYSTEM] 1 Minute collection window completed.");
    Serial.println("[SYSTEM] Shutting down Main CPU, DMA Engine, Wi-Fi, and RTC Peripherals...");
    Serial.flush();

    // 1. Release DMA and I2S ADC hardware
#if CAPTURE_MODE == MODE_I2S_ADC_DMA
    i2s_adc_disable(I2S_NUM);
    i2s_driver_uninstall(I2S_NUM);
#endif

    // 2. Shut down Wi-Fi radio to maximize battery efficiency
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // 3. Turn off RTC peripherals during deep sleep for maximum power savings
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);

    // 4. Configure Wakeup Source: RTC Timer ONLY
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_SEC * uS_TO_S_FACTOR);

    Serial.printf("[SYSTEM] Entering Deep Sleep. Sleeping for %d seconds...\n", TIME_TO_SLEEP_SEC);
    Serial.flush();

    // 5. Enter Deep Sleep Mode
    esp_deep_sleep_start();
}

// ======================================================================================
// 7. SETUP & MAIN LOOP
// ======================================================================================

void setup() {
    Serial.begin(3000000);
    while (!Serial && millis() < 1500);

    // Increment boot count & log wakeup cause
    ++bootCount;
    Serial.printf("\n======================================================\n");
    Serial.printf("ESP32 High-Speed 80kHz DMA Sampler — Boot Count: %d\n", bootCount);
    print_wakeup_reason();
    Serial.printf("======================================================\n");

    // Initialize Wi-Fi connection
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    uint32_t wifi_start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifi_start < 5000)) {
        delay(100);
    }

    // Initialize static header fields
    burst_frame.header.magic1 = 0xAA;
    burst_frame.header.magic2 = 0x55;
    burst_frame.header.sample_count = READINGS_PER_BURST;

    // Setup DMA hardware
#if CAPTURE_MODE == MODE_I2S_ADC_DMA
    setup_i2s_adc_dma();
#elif CAPTURE_MODE == MODE_SPI_DMA
    setup_spi_dma();
#endif

    // Record start time of the active 1-minute collection window
    active_start_time = millis();
}

void loop() {
    // Check if 1-minute active capture window has elapsed
    if (millis() - active_start_time >= ACTIVE_DURATION_MS) {
        prepare_and_enter_deep_sleep(); // Powers down and enters Deep Sleep
    }

    // 1. Fetch burst block directly from hardware DMA buffer
    read_dma_samples(dma_raw_buf, READINGS_PER_BURST);

    // 2. Perform Data Sanity Checks
    verify_data_sanity(dma_raw_buf, READINGS_PER_BURST);

    // 3. Format 3-Byte Readings & populate UDP Frame payload
    for (int i = 0; i < READINGS_PER_BURST; i++) {
        uint16_t val = dma_raw_buf[i];
        uint8_t msb = (val >> 8) & 0xFF;
        uint8_t lsb = val & 0xFF;

        burst_frame.payload[i].sync = SYNC_BYTE; // 0xAA
        burst_frame.payload[i].msb  = msb;
        burst_frame.payload[i].lsb  = lsb;
    }

    // 4. Send UDP Packet in burst mode if Wi-Fi is connected
    if (WiFi.status() == WL_CONNECTED) {
        burst_frame.header.sequence_num = global_sequence_num++;

        size_t payload_bytes = sizeof(BurstHeader) + (READINGS_PER_BURST * sizeof(Reading3Byte));
        burst_frame.crc16 = calculate_crc16((const uint8_t*)&burst_frame, payload_bytes);

        udp.beginPacket(UDP_DEST_IP, UDP_DEST_PORT);
        udp.write((const uint8_t*)&burst_frame, sizeof(BurstUDPFrame));
        udp.endPacket();
    }
}