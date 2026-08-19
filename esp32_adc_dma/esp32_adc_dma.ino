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
#include "secrets.h"
// Set SD_LOGGING_ENABLE to 1 if writing directly to an SD Card (SPI / SD_MMC)
#define SD_LOGGING_ENABLE 0
#if SD_LOGGING_ENABLE
#include "FS.h"
#include "SD.h"
#endif
// ----------------------------------------------------------------
// Configuration & Constants
// ----------------------------------------------------------------
#define I2S_PORT       I2S_NUM_0
#define SAMPLE_RATE    80000
#define DMA_BUF_LEN    1024
#define DMA_BUF_COUNT  4
#define ADC_CHANNEL    ADC1_CHANNEL_6   // GPIO34
uint16_t dmaSamples[DMA_BUF_LEN];
uint32_t totalSamples = 0;
uint32_t lastReport = 0;
// ----------------------------------------------------------------
// Data Structures
// ----------------------------------------------------------------
struct DmaBlock {              // one queue item per DMA read
    time_t   epochSec;
    uint16_t ms;
    uint32_t durationUs;       // wall-clock acquisition duration
    uint16_t count;
    uint16_t values[DMA_BUF_LEN];
};
struct FlatSample {            // flattened sample entry
    time_t   epochSec;
    uint16_t ms;
    uint16_t value;
};
enum class UploadResult : uint8_t {
    Ok,
    Retry,
    Drop
};
const size_t BATCH_SIZE = DMA_BUF_LEN;          // 1024 samples per batch
const size_t CSV_PAYLOAD_CAPACITY = 24576;      // ~1024 samples * ~20B + header (Much smaller than JSON)
const UBaseType_t SAMPLE_QUEUE_DEPTH = 8;
QueueHandle_t      sampleQueue;
WiFiClientSecure   netClient;
// Telemetry & Diagnostic counters
volatile uint32_t droppedBlocks = 0;
uint32_t          droppedUploads = 0;
uint32_t          uploadedSamples = 0;
uint32_t          lastUploadReport = 0;
#if SD_LOGGING_ENABLE
File csvFile;
#endif
// Function Declarations
bool appendText(char* out, size_t cap, size_t& len, const char* text);
bool appendSampleCsv(char* out, size_t cap, size_t& len, const FlatSample& sample);
bool buildCsvPayload(char* out, size_t cap, FlatSample* buf, size_t count, size_t& len);
UploadResult postBatchCsv(HTTPClient& http, FlatSample* buf, size_t count);
// ----------------------------------------------------------------
// CSV Payload Formatting helpers
// Format:
// device_id
// timestamp_sec,ms,adc_value
// ----------------------------------------------------------------
bool appendText(char* out, size_t cap, size_t& len, const char* text) {
    size_t textLen = strlen(text);
    if (len + textLen >= cap) return false;
    memcpy(out + len, text, textLen);
    len += textLen;
    out[len] = '\0';
    return true;
}
bool appendSampleCsv(char* out, size_t cap, size_t& len, const FlatSample& sample) {
    int written = snprintf(out + len, cap - len,
                           "%" PRId64 ",%u,%u\n",
                           (int64_t)sample.epochSec,
                           (unsigned)sample.ms,
                           (unsigned)sample.value);
    if (written < 0 || (size_t)written >= cap - len) return false;
    len += (size_t)written;
    return true;
}
bool buildCsvPayload(char* out, size_t cap, FlatSample* buf, size_t count, size_t& len) {
    len = 0;
    out[0] = '\0';
    // CSV Header row
    if (!appendText(out, cap, len, "timestamp_sec,ms,adc_value\n")) return false;
    for (size_t i = 0; i < count; i++) {
        if (!appendSampleCsv(out, cap, len, buf[i])) return false;
    }
    return true;
}
// ----------------------------------------------------------------
// HTTP Upload using CSV Payload
// ----------------------------------------------------------------
UploadResult postBatchCsv(HTTPClient& http, FlatSample* buf, size_t count) {
    if (WiFi.status() != WL_CONNECTED) {
        return UploadResult::Retry;
    }
    static char payload[CSV_PAYLOAD_CAPACITY];
    size_t payloadLen = 0;
    if (!buildCsvPayload(payload, sizeof(payload), buf, count, payloadLen)) {
        return UploadResult::Drop;
    }
    if (!http.begin(netClient, SERVER_URL)) {
        return UploadResult::Retry;
    }
    http.addHeader("Content-Type", "text/csv");
    http.addHeader("X-Device-ID", DEVICE_ID);
    http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
    http.addHeader("User-Agent", "esp32-i2s-csv/1.0");
    http.setTimeout(3000);
    int code = http.POST((uint8_t*)payload, payloadLen);
    bool ok = (code >= 200 && code < 300);
    http.end();
    if (ok) return UploadResult::Ok;
    if (code >= 400 && code < 500 && code != 408 && code != 429) return UploadResult::Drop;
    return UploadResult::Retry;
}
// ----------------------------------------------------------------
// Background Task: Process Queue & Save/Upload CSV
// ----------------------------------------------------------------
void uploadTask(void* param) {
    static FlatSample buffer[BATCH_SIZE];
    HTTPClient http;
    http.setReuse(true);
    for (;;) {
        DmaBlock block;
        size_t count = 0;
        if (xQueueReceive(sampleQueue, &block, portMAX_DELAY) == pdTRUE && block.count > 0) {
            double usPerSample = (double)block.durationUs / block.count;
            for (uint16_t i = 0; i < block.count; i++) {
                int64_t offsetUs = (int64_t)llround(i * usPerSample);
                time_t  sampleSec = block.epochSec;
                int64_t sampleMs  = (int64_t)block.ms + offsetUs / 1000;
                while (sampleMs >= 1000) { sampleMs -= 1000; sampleSec += 1; }
                while (sampleMs < 0)     { sampleMs += 1000; sampleSec -= 1; }
                buffer[count++] = { sampleSec, (uint16_t)sampleMs, block.values[i] };
            }
        }
        if (count > 0) {
            #if SD_LOGGING_ENABLE
            if (csvFile) {
                for (size_t i = 0; i < count; i++) {
                    csvFile.printf("%" PRId64 ",%u,%u\n", (int64_t)buffer[i].epochSec, buffer[i].ms, buffer[i].value);
                }
                csvFile.flush();
            }
            #else
            UploadResult result = postBatchCsv(http, buffer, count);
            if (result == UploadResult::Ok) {
                uploadedSamples += count;
            } else if (result == UploadResult::Drop) {
                droppedUploads += count;
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            #endif
        }
    }
}
// ----------------------------------------------------------------
// WiFi & Time Sync
// ----------------------------------------------------------------
void setupTime() {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        if (millis() - start > 15000) {
            ESP.restart();
        }
    }
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo;
    start = millis();
    while (!getLocalTime(&timeinfo)) {
        delay(250);
        if (millis() - start > 10000) {
            configTime(19800, 0, "time.google.com", "time.cloudflare.com");
            start = millis();
        }
    }
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
    Serial.begin(921600);
    delay(500);
    #if !SD_LOGGING_ENABLE
    setupTime();
    netClient.setInsecure();
    netClient.setHandshakeTimeout(10);
    #else
    if (SD.begin()) {
        csvFile = SD.open("/adc_samples.csv", FILE_APPEND);
        if (csvFile && csvFile.size() == 0) {
            csvFile.println("timestamp_sec,ms,adc_value");
        }
    }
    #endif
    sampleQueue = xQueueCreate(SAMPLE_QUEUE_DEPTH, sizeof(DmaBlock));
    xTaskCreatePinnedToCore(uploadTask, "uploadTask", 16384, nullptr, 1, nullptr, 1);
    setupAdcDma();
}
// ----------------------------------------------------------------
// Loop - DMA Producer & Sample Rate Monitor
// ----------------------------------------------------------------
void loop() {
    size_t bytesRead = 0;
    uint32_t startMicros = micros();
    i2s_read(I2S_PORT, dmaSamples, sizeof(dmaSamples), &bytesRead, portMAX_DELAY);
    uint32_t elapsed = micros() - startMicros;
    uint32_t sampleCount = bytesRead / sizeof(uint16_t);
    DmaBlock block;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    block.epochSec = tv.tv_sec;
    block.ms = tv.tv_usec / 1000;
    block.durationUs = elapsed;
    block.count = (uint16_t)sampleCount;
    for (uint32_t i = 0; i < sampleCount; i++) {
        block.values[i] = dmaSamples[i] & 0x0FFF;
    }
    if (xQueueSend(sampleQueue, &block, 0) != pdTRUE) {
        droppedBlocks++;
    }
    totalSamples += sampleCount;
    // ------------------------------------------------------------
    // Sample Rate Output (EVERY 1 SECOND) - ONLY PRINT SAMPLE RATE
    // ------------------------------------------------------------
    uint32_t now = millis();
    if (now - lastReport >= 1000) {
        float rate = totalSamples * 1000.0f / (now - lastReport);

        // Clean single-line output printing ONLY the sample rate
        Serial.printf("Sample Rate: %.1f Hz\n", rate);
        totalSamples = 0;
        lastReport = now;
    }
}
