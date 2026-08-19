/*
 * ESP32 Continuous ADC DMA Streaming using modern esp_adc/adc_continuous.h
 *
 * Migration from deprecated i2s_adc driver to maintained ESP-IDF continuous ADC API.
 * Solves legacy I2S clock-divider halving issues and provides exact hardware sampling frequency.
 * Compatible with ESP-IDF 4.4+ / 5.x and Arduino-ESP32 2.0+ / 3.0+
 */

#include <Arduino.h>
#include "esp_adc/adc_continuous.h"

// ----------------------------------------------------------------
// Configuration Parameters
// ----------------------------------------------------------------
#define TARGET_SAMPLE_RATE 80000            // Target sample rate: 80 kHz
#define SAMPLE_RATE_MULT   2.0              // ESP32 DIG ADC HAL hardware clock prescaler factor
#define CONFIGURED_RATE    ((uint32_t)(TARGET_SAMPLE_RATE * SAMPLE_RATE_MULT))

#define ADC_FRAME_SIZE    1024             // Bytes per DMA conversion frame (512 samples)
#define ADC_STORE_BUF_SZ  4096             // Total Ring Buffer Size (bytes)
#define BAUDRATE          2000000          // 2 Mbaud Serial Speed

// Channel Configuration (GPIO34 = ADC1 Channel 6)
#define ADC_UNIT          ADC_UNIT_1
#define ADC_CHANNEL       ADC_CHANNEL_6
#define ADC_ATTEN         ADC_ATTEN_DB_11

adc_continuous_handle_t adc_handle = NULL;
uint8_t frameBuffer[ADC_FRAME_SIZE];

// ----------------------------------------------------------------
// ADC Continuous Driver Initialization
// ----------------------------------------------------------------
void setupAdcContinuous() {
    // 1. Create ADC Continuous Driver Handle
    adc_continuous_handle_cfg_t handle_cfg = {};
    handle_cfg.max_store_buf_size = ADC_STORE_BUF_SZ;
    handle_cfg.conv_frame_size    = ADC_FRAME_SIZE;
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    // 2. Pattern Configuration for Single Channel Sampling
    adc_digi_pattern_config_t adc_pattern[1] = {};
    adc_pattern[0].atten     = ADC_ATTEN;
    adc_pattern[0].channel   = ADC_CHANNEL;
    adc_pattern[0].unit      = ADC_UNIT;
    adc_pattern[0].bit_width = ADC_BITWIDTH_12;

    // 3. Continuous ADC Controller Config
    // Note: Configured sample_freq_hz is doubled to 160 kHz to achieve exact 80 kHz hardware sampling rate
    adc_continuous_config_t dig_cfg = {};
    dig_cfg.pattern_num    = 1;
    dig_cfg.adc_pattern    = adc_pattern;
    dig_cfg.sample_freq_hz = CONFIGURED_RATE;
    dig_cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
    dig_cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));

    // 4. Start Continuous Conversion
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

// ----------------------------------------------------------------
// Setup
// ----------------------------------------------------------------
void setup() {
    Serial.begin(BAUDRATE);
    delay(500);

    setupAdcContinuous();
}

// ----------------------------------------------------------------
// Loop - Stream DMA Frames over High-Speed Serial
// ----------------------------------------------------------------
void loop() {
    uint32_t ret_num = 0;
    esp_err_t ret = adc_continuous_read(adc_handle, frameBuffer, ADC_FRAME_SIZE, &ret_num, 0);

    if (ret == ESP_OK && ret_num > 0) {
        // Stream raw DMA conversion frame over 2 Mbaud UART
        Serial.write(frameBuffer, ret_num);
    }
}
