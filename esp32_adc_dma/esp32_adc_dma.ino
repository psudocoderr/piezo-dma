#include <Arduino.h>
#include <driver/i2s.h>
#include <driver/adc.h>

#define I2S_PORT        I2S_NUM_0

#define SAMPLE_RATE     80000

#define DMA_BUF_LEN     2048
#define DMA_BUF_COUNT   8

#define ADC_CHANNEL     ADC1_CHANNEL_6      // GPIO34

uint16_t dmaBuffer[DMA_BUF_LEN];

void setup()
{
    Serial.begin(921600);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(
        ADC_CHANNEL,
        ADC_ATTEN_DB_11
    );

    i2s_config_t i2s_config =
    {
        .mode = (i2s_mode_t)(
            I2S_MODE_MASTER |
            I2S_MODE_RX |
            I2S_MODE_ADC_BUILT_IN
        ),

        .sample_rate = SAMPLE_RATE,

        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,

        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,

        .communication_format = I2S_COMM_FORMAT_STAND_I2S,

        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,

        .dma_buf_count = DMA_BUF_COUNT,

        .dma_buf_len = DMA_BUF_LEN,

        .use_apll = false,

        .tx_desc_auto_clear = false,

        .fixed_mclk = 0
    };

    i2s_driver_install(
        I2S_PORT,
        &i2s_config,
        0,
        NULL
    );

    i2s_set_adc_mode(
        ADC_UNIT_1,
        ADC_CHANNEL
    );

    i2s_adc_enable(I2S_PORT);

    Serial.println("Started");
}

void loop()
{
    size_t bytesRead = 0;

    esp_err_t err = i2s_read(
        I2S_PORT,
        dmaBuffer,
        sizeof(dmaBuffer),
        &bytesRead,
        portMAX_DELAY
    );

    if (err == ESP_OK && bytesRead > 0)
    {
        uint16_t samples = bytesRead / sizeof(uint16_t);
        // Serial.write((uint8_t *)dmaBuffer, bytesRead);

        for (uint16_t i = 0; i < samples; i++)
        {
            uint16_t adcValue = dmaBuffer[i] & 0x0FFF;
            Serial.println(adcValue);
        }
    }
}
