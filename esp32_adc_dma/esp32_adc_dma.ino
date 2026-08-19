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

// ----------------------------------------------------------------
// Operating Mode Toggle
// Set TELEMETRY_VIEWER to 1 for Direct Binary Serial.write Streaming (2 Mbaud)
// Set TELEMETRY_VIEWER to 0 for Background HTTP Upload Mode
// ----------------------------------------------------------------
#define TELEMETRY_VIEWER 1

#if TELEMETRY_VIEWER
#define SERIAL_BAUD_RATE 2000000
#else
#define SERIAL_BAUD_RATE 921600
#endif

// ----------------------------------------------------------------
// I2S / ADC DMA
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
// Binary Serial Packet Header (used when streaming binary)
// ----------------------------------------------------------------
#pragma pack(push, 1)
struct BinaryPacketHeader {
    uint8_t  magic[2];     // {0xAA, 0x55}
    uint16_t count;        // Number of 16-bit samples following
    uint32_t epochSec;     // Unix timestamp (seconds)
    uint16_t ms;           // Milliseconds (0-999)
    uint32_t durationUs;   // Block acquisition duration in microseconds
};
#pragma pack(pop)

// ----------------------------------------------------------------
// HTTP batch upload
// ----------------------------------------------------------------
struct DmaBlock {              // one queue item per DMA read
    time_t   epochSec;
    uint16_t ms;
    uint32_t durationUs;       // wall-clock time the block took to acquire
    uint16_t count;
    uint16_t values[DMA_BUF_LEN];
};

struct FlatSample {            // one entry in the upload buffer
    time_t   epochSec;
    uint16_t ms;
    uint16_t value;
};

enum class UploadResult : uint8_t
{
    Ok,
    Retry,
    Drop
};

const size_t BATCH_SIZE = DMA_BUF_LEN;          // one full DMA block per POST
const size_t JSON_PAYLOAD_CAPACITY = 40960;     // ~1024 samples * ~35B + headroom

const UBaseType_t SAMPLE_QUEUE_DEPTH = 6;

QueueHandle_t      sampleQueue;
WiFiClientSecure   netClient;

// Upload diagnostics
volatile uint32_t droppedBlocks = 0;
uint32_t          droppedUploads = 0;
uint32_t          uploadedSamples = 0;
uint32_t          lastUploadReport = 0;

bool appendText(char* out, size_t cap, size_t& len, const char* text);
bool appendSampleJson(char* out, size_t cap, size_t& len, const FlatSample& sample);
bool buildPayload(char* out, size_t cap, FlatSample* buf, size_t count, size_t& len);
UploadResult postBatch(HTTPClient& http, FlatSample* buf, size_t count);

bool appendText(char* out, size_t cap, size_t& len, const char* text)
{
    size_t textLen = strlen(text);
    if (len + textLen >= cap)
        return false;

    memcpy(out + len, text, textLen);
    len += textLen;
    out[len] = '\0';
    return true;
}

bool appendSampleJson(char* out, size_t cap, size_t& len, const FlatSample& sample)
{
    int written = snprintf(out + len,
                           cap - len,
                           "{\"t\":%" PRId64 ",\"ms\":%u,\"v\":%u}",
                           (int64_t)sample.epochSec,
                           (unsigned)sample.ms,
                           (unsigned)sample.value);

    if (written < 0 || (size_t)written >= cap - len)
        return false;

    len += (size_t)written;
    return true;
}

bool buildPayload(char* out, size_t cap, FlatSample* buf, size_t count, size_t& len)
{
    len = 0;
    out[0] = '\0';

    if (!appendText(out, cap, len, "{\"device\":\""))
        return false;
    if (!appendText(out, cap, len, DEVICE_ID))
        return false;
    if (!appendText(out, cap, len, "\",\"samples\":["))
        return false;

    for (size_t i = 0; i < count; i++)
    {
        if (i > 0 && !appendText(out, cap, len, ","))
            return false;
        if (!appendSampleJson(out, cap, len, buf[i]))
            return false;
    }

    return appendText(out, cap, len, "]}");
}

UploadResult postBatch(HTTPClient& http, FlatSample* buf, size_t count)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi down, skipping upload");
        return UploadResult::Retry;
    }

    static char payload[JSON_PAYLOAD_CAPACITY];
    size_t payloadLen = 0;

    if (!buildPayload(payload, sizeof(payload), buf, count, payloadLen))
    {
        Serial.printf("Upload payload too large for %u-byte buffer, count=%u\n",
                      (unsigned)sizeof(payload),
                      (unsigned)count);
        return UploadResult::Drop;
    }

    Serial.printf("[HTTP] Payload: %u bytes | Free heap: %u | Largest block: %u bytes\n",
                  (unsigned)payloadLen,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!http.begin(netClient, SERVER_URL))
    {
        Serial.println("HTTP begin failed");
        return UploadResult::Retry;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
    http.addHeader("User-Agent", "pulse-esp32-dma/1.0");
    http.setTimeout(5000);

    int code = http.POST((uint8_t*)payload, payloadLen);
    bool ok = (code >= 200 && code < 300);
    String response;

    if (ok)
    {
        Serial.printf("Uploaded %u samples (HTTP %d)\n", (unsigned)count, code);
    }
    else if (code < 0)
    {
        char errBuf[128] = {0};
        netClient.lastError(errBuf, sizeof(errBuf));
        Serial.printf("Upload failed: HTTP Code = %d (%s). SSL error: '%s'\n",
                      code, http.errorToString(code).c_str(), errBuf);
    }
    else
    {
        Serial.printf("Upload failed: HTTP Code = %d\n", code);
        response = http.getString();
        if (response.length() > 0)
        {
            Serial.print("Server response: ");
            Serial.println(response);
        }
    }

    http.end();

    if (ok)
        return UploadResult::Ok;

    if (code == 530 && response.indexOf("1033") >= 0)
    {
        Serial.println("Cloudflare tunnel/origin error 1033; dropping this batch until server is fixed");
        return UploadResult::Drop;
    }

    if (code >= 400 && code < 500 && code != 408 && code != 429)
        return UploadResult::Drop;

    return UploadResult::Retry;
}

void uploadTask(void* param)
{
    static FlatSample buffer[BATCH_SIZE];

    HTTPClient http;
    http.setReuse(true);

    for (;;)
    {
        DmaBlock block;
        size_t count = 0;

        if (xQueueReceive(sampleQueue, &block, portMAX_DELAY) == pdTRUE && block.count > 0)
        {
            double usPerSample = (double)block.durationUs / block.count;

            for (uint16_t i = 0; i < block.count; i++)
            {
                int64_t offsetUs = (int64_t)llround(i * usPerSample);
                time_t  sampleSec = block.epochSec;
                int64_t sampleMs  = (int64_t)block.ms + offsetUs / 1000;

                while (sampleMs >= 1000) { sampleMs -= 1000; sampleSec += 1; }
                while (sampleMs < 0)     { sampleMs += 1000; sampleSec -= 1; }

                buffer[count++] = { sampleSec, (uint16_t)sampleMs, block.values[i] };
            }
        }

        if (count > 0)
        {
            UploadResult result = postBatch(http, buffer, count);

            if (result == UploadResult::Ok)
            {
                uploadedSamples += count;
            }
            else if (result == UploadResult::Drop)
            {
                droppedUploads += count;
                Serial.printf("Dropping %u samples after non-retryable upload failure\n",
                              (unsigned)count);
            }
            else
            {
                Serial.println("Retrying upload...");
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        uint32_t now = millis();
        if (now - lastUploadReport >= 5000)
        {
            Serial.printf("[Upload] sent=%u dropped=%u in last 5s (%.1f samples/sec)\n",
                          (unsigned)uploadedSamples,
                          (unsigned)droppedUploads,
                          uploadedSamples / 5.0f);
            uploadedSamples = 0;
            droppedUploads = 0;
            lastUploadReport = now;
        }
    }
}

void setupTime()
{
    Serial.print("Connecting to WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - start > 20000) {
            Serial.println("\nWiFi timeout, restarting...");
            ESP.restart();
        }
    }
    Serial.println("\nWiFi Connected");

    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    struct tm timeinfo;
    Serial.print("Synchronizing time");

    start = millis();
    while (!getLocalTime(&timeinfo)) {
        Serial.print(".");
        delay(500);
        if (millis() - start > 15000) {
            Serial.println("\nNTP timeout, retrying with backup server...");
            configTime(19800, 0, "time.google.com", "time.cloudflare.com");
            start = millis();
        }
    }
    Serial.println("\nTime synchronized");
}

//------------------------------------------------------------
// ADC DMA
//------------------------------------------------------------
void setupAdcDma()
{
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

//------------------------------------------------------------
// Setup
//------------------------------------------------------------
void setup()
{
    Serial.setTxBufferSize(8192); // Large TX buffer for high-speed binary writes
    Serial.begin(SERIAL_BAUD_RATE);
    delay(1000);

    #if !TELEMETRY_VIEWER
    Serial.println("--------------------------------");
    Serial.println("ESP32 ADC DMA -> HTTP Uploader");
    Serial.printf("Sample Rate      : %d Hz\n", SAMPLE_RATE);
    Serial.println("GPIO34 -> ADC1_CHANNEL_6");
    Serial.print("HTTP URL         : ");
    Serial.println(SERVER_URL);
    Serial.println("--------------------------------");

    setupTime();

    netClient.setInsecure();
    netClient.setHandshakeTimeout(15);

    sampleQueue = xQueueCreate(SAMPLE_QUEUE_DEPTH, sizeof(DmaBlock));
    xTaskCreatePinnedToCore(uploadTask, "uploadTask", 16384, nullptr, 1, nullptr, 1);
    #endif

    setupAdcDma();
}

//------------------------------------------------------------
// Loop - High-Speed Binary Streaming via Serial.write
//------------------------------------------------------------
void loop()
{
    size_t bytesRead = 0;
    uint32_t startMicros = micros();

    i2s_read(
        I2S_PORT,
        dmaSamples,
        sizeof(dmaSamples),
        &bytesRead,
        portMAX_DELAY);

    uint32_t elapsed = micros() - startMicros;
    uint32_t sampleCount = bytesRead / sizeof(uint16_t);

    DmaBlock block;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    block.epochSec = tv.tv_sec;
    block.ms = tv.tv_usec / 1000;
    block.durationUs = elapsed;
    block.count = (uint16_t)sampleCount;

    for (uint32_t i = 0; i < sampleCount; i++)
        block.values[i] = dmaSamples[i] & 0x0FFF;

    #if TELEMETRY_VIEWER
    // High-speed binary transmission: Write framed packet (Header + Raw 16-bit uint16 values)
    BinaryPacketHeader header;
    header.magic[0] = 0xAA;
    header.magic[1] = 0x55;
    header.count = (uint16_t)sampleCount;
    header.epochSec = (uint32_t)tv.tv_sec;
    header.ms = (uint16_t)(tv.tv_usec / 1000);
    header.durationUs = elapsed;

    Serial.write((const uint8_t*)&header, sizeof(header));
    Serial.write((const uint8_t*)block.values, sampleCount * sizeof(uint16_t));
    #else
    if (xQueueSend(sampleQueue, &block, 0) != pdTRUE)
    {
        droppedBlocks++;
    }

    totalSamples += sampleCount;

    uint32_t now = millis();
    if (now - lastReport >= 1000)
    {
        float rate = totalSamples * 1000.0f / (now - lastReport);

        Serial.println("--------------------------------");
        Serial.printf("DMA Rate     : %.1f samples/sec (target %d)\n", rate, SAMPLE_RATE);
        Serial.printf("Queue Used   : %u / %u\n",
                      (unsigned)(SAMPLE_QUEUE_DEPTH - uxQueueSpacesAvailable(sampleQueue)),
                      (unsigned)SAMPLE_QUEUE_DEPTH);
        Serial.printf("Dropped      : %u blocks\n", (unsigned)droppedBlocks);
        Serial.printf("Free Heap    : %u bytes\n", (unsigned)ESP.getFreeHeap());
        Serial.println("--------------------------------");

        totalSamples = 0;
        lastReport = now;
    }
    #endif
}