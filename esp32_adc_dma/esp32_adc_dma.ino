#include <Arduino.h>
#include "esp_adc/adc_continuous.h"
#include "adc_protocol.h"

// ============================================================================
// Configuration Parameters
// ============================================================================
#define TARGET_SAMPLE_RATE 80000 // Target ADC sampling rate in Hz (80 kS/s)
#define SAMPLE_RATE_MULT   1.0   
#define CONFIGURED_RATE    ((uint32_t)(TARGET_SAMPLE_RATE * SAMPLE_RATE_MULT))

// DMA frame configuration
#define DMA_CONV_FRAME_SZ  512   // DMA interrupt buffer frame size (bytes)
#define ADC_FRAME_SIZE     (ADC_DEFAULT_SAMPLES * sizeof(adc_digi_output_data_t)) // 2048 bytes
#define ADC_STORE_BUF_SZ   8192  // 8192 bytes ring buffer in driver

#define ADC_UNIT           ADC_UNIT_1
#define ADC_CHANNEL        ADC_CHANNEL_6 // GPIO34 on ESP32
#define ADC_ATTEN          ADC_ATTEN_DB_11 // 0-3.3V range

#define LED_PIN            2     // Built-in LED on most ESP32 boards

// ============================================================================
// Global Objects & Buffers
// ============================================================================
adc_continuous_handle_t adc_handle = NULL;

// Raw DMA reading frame buffer
static uint8_t dma_read_buf[ADC_FRAME_SIZE];

// Decoded 12-bit uint16 payload buffer
static uint16_t sample_payload[ADC_DEFAULT_SAMPLES];

static adc_packet_header_t header;
static uint32_t packet_sequence = 0;

// ============================================================================
// ADC Continuous DMA Initialization
// ============================================================================
void setupAdcContinuous() {
    adc_continuous_handle_cfg_t handle_cfg = {};
    handle_cfg.max_store_buf_size = ADC_STORE_BUF_SZ;
    handle_cfg.conv_frame_size = DMA_CONV_FRAME_SZ;

    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    adc_digi_pattern_config_t pattern[1] = {};
    pattern[0].atten = ADC_ATTEN;
    pattern[0].channel = ADC_CHANNEL;
    pattern[0].unit = ADC_UNIT;
    pattern[0].bit_width = ADC_BITWIDTH_12;

    adc_continuous_config_t cfg = {};
    cfg.pattern_num = 1;
    cfg.adc_pattern = pattern;
    cfg.sample_freq_hz = CONFIGURED_RATE;
    cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;

    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

// ============================================================================
// Arduino Setup & Loop
// ============================================================================
void setup() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Initialize Hardware Serial at high baud rate (2 Mbaud)
    Serial.begin(ADC_BAUDRATE);
    Serial.setTxBufferSize(ADC_FRAME_SIZE * 2);
    delay(500);

    setupAdcContinuous();
}

void loop() {
    uint32_t bytesRead = 0;

    // Read continuous raw DMA block from driver (timeout = 100 ms)
    esp_err_t ret = adc_continuous_read(
        adc_handle,
        dma_read_buf,
        ADC_FRAME_SIZE,
        &bytesRead,
        100 // 100 ms timeout to ensure DMA buffer populates
    );

    if (ret == ESP_OK && bytesRead > 0) {
        uint32_t num_dma_samples = bytesRead / sizeof(adc_digi_output_data_t);
        if (num_dma_samples == 0) {
            return;
        }

        adc_digi_output_data_t *dma_samples = (adc_digi_output_data_t *)dma_read_buf;

        // Decode TYPE1 DMA words -> uint16_t 12-bit ADC values
        for (uint32_t i = 0; i < num_dma_samples; i++) {
            // Extract 12-bit ADC value (bits [11:0])
            sample_payload[i] = (uint16_t)(dma_samples[i].type1.data & 0x0FFF);
        }

        uint32_t payload_bytes = num_dma_samples * sizeof(uint16_t);

        // Construct Binary Packet Header
        header.sync = ADC_SYNC_WORD;
        header.sequence = packet_sequence++;
        header.sample_count = (uint16_t)num_dma_samples;
        header.timestamp_us = micros();
        header.crc16 = calculate_crc16((const uint8_t *)sample_payload, payload_bytes);

        // Transmit Packet Header followed by Payload over Serial
        Serial.write((const uint8_t *)&header, sizeof(header));
        Serial.write((const uint8_t *)sample_payload, payload_bytes);

        // Toggle LED every 100 packets (~1.2 seconds) as heartbeat indicator
        if ((packet_sequence % 100) == 0) {
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        }
    }
}
