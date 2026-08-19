/*
 * ESP32 High-Speed Continuous ADC DMA Binary Streaming over Serial
 *
 * Features:
 * 1. Uses I2S DMA continuous ADC sampling (hardware DMA in background without CPU overhead).
 * 2. High-speed 2,000,000 baud rate (2 Mbaud).
 * 3. Streams raw 16-bit binary samples formatted as [High Byte, Low Byte].
 * 4. WiFi, HTTP, and SD logging code completely removed for maximum performance.
 */

#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <inttypes.h>

// ----------------------------------------------------------------
// Configuration Parameters
// ----------------------------------------------------------------
#define I2S_PORT       I2S_NUM_0
#define SAMPLE_RATE    80000            // 80 kHz sampling rate
#define DMA_BUF_LEN    1024             // DMA buffer length (samples)
#define DMA_BUF_COUNT  4                // Number of DMA buffers
#define ADC_CHANNEL    ADC1_CHANNEL_6   // GPIO34

#define BAUDRATE       2000000          // 2 Mbaud Serial Speed

// DMA input buffer & packed binary transmit buffer
uint16_t dmaSamples[DMA_BUF_LEN];
uint8_t  txBuffer[DMA_BUF_LEN * 2];     // 2 bytes per sample (High byte, Low byte)

// ----------------------------------------------------------------
// Helper: Convert single 16-bit ADC sample into 2-byte binary array
// ----------------------------------------------------------------
inline void packSampleBinary(uint16_t analogSample, uint8_t buffer[2]) {
    buffer[0] = (uint8_t)((analogSample >> 8) & 0xFF); // High byte
    buffer[1] = (uint8_t)(analogSample & 0xFF);        // Low byte
}

// ----------------------------------------------------------------
// ADC DMA Initialization
// ----------------------------------------------------------------
void setupAdcDma() {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_adc_mode(ADC_UNIT_1, ADC_CHANNEL);
    i2s_adc_enable(I2S_PORT);
}

// ----------------------------------------------------------------
// Setup
// ----------------------------------------------------------------
void setup() {
    // Initialize High-Speed Serial Port at 2 Mbaud
    Serial.begin(BAUDRATE);
    delay(500);

    // Setup I2S Continuous Hardware DMA ADC
    setupAdcDma();
}

// ----------------------------------------------------------------
// Loop - High-Speed Binary Streaming
// ----------------------------------------------------------------
void loop() {
    
    size_t bytesRead = 0;

    // Read filled DMA buffer from I2S continuous hardware receiver
    esp_err_t err = i2s_read(I2S_PORT, dmaSamples, sizeof(dmaSamples), &bytesRead, portMAX_DELAY);

    if (err == ESP_OK && bytesRead > 0) {
        uint32_t sampleCount = bytesRead / sizeof(uint16_t);
        size_t txIndex = 0;

        // Pack each 12-bit ADC sample into binary block [High Byte, Low Byte]
        // for (uint32_t i = 0; i < sampleCount; i++) {
        //     uint16_t analogSample = dmaSamples[i] & 0x0FFF; // 12-bit ADC value (0 - 4095)

        //     // Format sample into buffer: buffer[0] high byte, buffer[1] low byte
        //     packSampleBinary(analogSample, &txBuffer[txIndex]);
        //     txIndex += 2;
        // }    

        // // Stream high-speed binary block over 2M baud UART
        // Serial.write(txBuffer, txIndex);
        
        Serial.write((uint8_t *)dmaSamples, bytesRead); //uncomment this when not debugging
        // size_t sent = Serial.write((uint8_t *)dmaSamples, bytesRead);
        // if (sent != bytesRead) {
        //     Serial.println("Overflow!!");
        // }
    }
}
