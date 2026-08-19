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
// I2S / ADC DMA
// ----------------------------------------------------------------
#define I2S_PORT       I2S_NUM_0
#define SAMPLE_RATE    80000
#define DMA_BUF_LEN    1024
#define DMA_BUF_COUNT  4
#define ADC_CHANNEL    ADC1_CHANNEL_6   // GPIO34
#define TELEMETRY_VIEWER 0

// ----------------------------------------------------------------
// Binary frame format for TELEMETRY_VIEWER mode (see chat for rationale).
// One frame per DMA block, little-endian, no padding (14-byte header):
//
//   uint16_t magic       0x55AA sentinel (bytes: 0xAA, 0x55 in little-endian)
//   uint16_t count        number of uint16_t sample values following
//   uint32_t epochSec     block start time in seconds
//   uint16_t ms            block start time, milliseconds
//   uint32_t durationUs   wall-clock time the block took to acquire
//   ... count * uint16_t sample values ...
// ----------------------------------------------------------------
#pragma pack(push, 1)
struct ViewerFrameHeader {
    uint16_t magic;
    uint16_t count;
    uint32_t epochSec;
    uint16_t ms;
    uint32_t durationUs;
};
#pragma pack(pop)

uint16_t dmaSamples[DMA_BUF_LEN];

uint32_t totalSamples = 0;
uint32_t lastReport = 0;

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
// NOTE: at 80kHz each block arrives every 12.8ms (1024 samples / 80000).
// uploadTask has to finish JSON-encoding *and* the full HTTPS POST round
// trip inside that window on average, or the queue backs up and blocks
// start dropping (see droppedBlocks below). That's a very tight budget
// for TLS+HTTP even with connection reuse. If you're seeing the same
// rate shortfall in this path (TELEMETRY_VIEWER=0), the fix is the same
// idea as the viewer path - cut bytes/sample (binary payload instead of
// JSON), and/or amortize per-request overhead by batching multiple
// blocks per POST, and/or decimate on-device if you don't need every
// raw sample, and/or move to a persistent connection (WebSocket/raw
// TCP) instead of discrete HTTPS POSTs.
const size_t JSON_PAYLOAD_CAPACITY = 40960;     // ~1024 samples * ~35B + headroom

// Queue depth is RAM-bound, not rate-bound: each DmaBlock is ~2KB, so this
// buys ~6 blocks (~75ms) of headroom before a slow POST starts dropping
// blocks. See note at the bottom of the chat response about the rate
// mismatch between 80kHz acquisition and HTTPS upload throughput.
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

//------------------------------------------------------------
// HTTP batch upload - JSON schema unchanged:
// {"device":"...","samples":[{"t":<epoch>,"ms":<0-999>,"v":<adc>},...]}
//------------------------------------------------------------
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

    // Largest CONTIGUOUS block matters more than total free heap for TLS -
    // mbedTLS needs one big chunk (~16KB per direction), not scattered ones.
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
    http.setTimeout(5000); // allow headroom for TLS + server write

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

    http.end();   // always release the connection + TLS context, success or fail

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

//------------------------------------------------------------
// Upload task: one DMA block in, one POST out. Each block already IS a
// full batch (BATCH_SIZE == DMA_BUF_LEN), so unlike the old BLE notify
// path there's no need to coalesce multiple queue items before sending.
//------------------------------------------------------------
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
            // Spread each sample's timestamp linearly across the time the
            // block actually took to acquire, instead of stamping every
            // sample in the block with the same millisecond.
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

        // ---- Upload rate report (every 5 sec) ----
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

//------------------------------------------------------------
// WiFi + NTP
//------------------------------------------------------------
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
            start = millis(); // give the new servers a fresh window
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

    Serial.println("ADC DMA started");
}

//------------------------------------------------------------
// Setup
//------------------------------------------------------------
void setup()
{
    // ASCII line-based printing (old loop() code) tops out around
    // 15-20k lines/sec even at high baud - nowhere near 80kHz. Binary
    // framing (see loop()) needs roughly 1.6Mbaud minimum just to fit
    // raw 16-bit samples at 80,000/sec (80000 * 2 bytes * 10 bits/byte),
    // so the baud rate has to go up too, not just the encoding.
    // setTxBufferSize must be called before begin(); a bigger ring
    // buffer absorbs scheduling jitter between DMA blocks arriving and
    // the UART draining them.
    //
    // Test for framing errors before trusting a given rate - CP2102N /
    // FTDI bridges are generally fine well past 2,000,000; CH340-based
    // boards tend to be more hit-or-miss above ~1,500,000. Push higher
    // (3,000,000+) if your hardware supports it for more headroom.
    Serial.setTxBufferSize(4096);
    Serial.begin(2000000); // was 921600 / previously tried 1000000 / 115200
    delay(1000);

    #if !TELEMETRY_VIEWER
    Serial.println("--------------------------------");
    Serial.println("ESP32 ADC DMA -> HTTP Uploader");
    Serial.printf("Sample Rate      : %d Hz\n", SAMPLE_RATE);
    Serial.println("GPIO34 -> ADC1_CHANNEL_6");
    Serial.print("HTTP URL         : ");
    Serial.println(SERVER_URL);
    Serial.println("--------------------------------");
    #endif

    #if !TELEMETRY_VIEWER
    // WiFi + NTP are only needed for the HTTPS upload path. setupTime()
    // blocks (and can restart the board) waiting on a WiFi connection,
    // so it was previously delaying/blocking ADC startup even in viewer
    // mode, where there's no network dependency at all - if WiFi wasn't
    // reachable, the board would sit here retrying and never reach
    // setupAdcDma()/loop(), and the binary stream would never start.
    setupTime();

    // TLS requires setInsecure() or a pinned CA. Using setInsecure() for throughput.
    netClient.setInsecure();
    netClient.setHandshakeTimeout(15);

    sampleQueue = xQueueCreate(SAMPLE_QUEUE_DEPTH, sizeof(DmaBlock));

    // Stack 16384: TLS handshake needs ~10 KB of stack on ESP32
    xTaskCreatePinnedToCore(uploadTask, "uploadTask", 16384, nullptr, 1, nullptr, 1);
    #else
    // Viewer mode: no WiFi/NTP, so gettimeofday()-based epochSec/ms in
    // loop() will just be relative-to-boot (starts near 0), not real
    // wall-clock time. That's fine for a local capture where you mostly
    // care about durationUs/sample values - flip TELEMETRY_VIEWER off
    // if you need accurate epoch timestamps.
    Serial.println("TELEMETRY_VIEWER mode: skipping WiFi/NTP, starting ADC DMA immediately.");
    #endif

    setupAdcDma();

    Serial.printf("[Boot] Free heap after init: %u | Largest block: %u bytes\n",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

//------------------------------------------------------------
// Loop - DMA producer. Runs on the default Arduino loop task (core 1);
// uploadTask (core 1 also, separate task) drains the queue independently
// so a slow POST never blocks the next DMA read.
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
    // Binary frame instead of per-sample ASCII println(). At 80kHz,
    // 1024-sample blocks arrive every 12.8ms; println() per sample costs
    // several bytes plus a full format+write call each time and can't
    // keep up at any reasonable baud. The DMA keeps sampling at the true
    // configured rate regardless of how fast this loop drains it, so a
    // slow consumer here just means most samples get silently
    // overwritten before we ever read them - that's the actual source
    // of the rate falling short, not the ADC/DMA itself.
    ViewerFrameHeader hdr;
    hdr.magic      = 0x55AA; // 0xAA, 0x55 in little-endian
    hdr.count      = (uint16_t)sampleCount;
    hdr.epochSec   = (uint32_t)block.epochSec;
    hdr.ms         = block.ms;
    hdr.durationUs = block.durationUs;

    Serial.write((const uint8_t*)&hdr, sizeof(hdr));
    Serial.write((const uint8_t*)block.values, sampleCount * sizeof(uint16_t));
    #endif

    #if !TELEMETRY_VIEWER
    if (xQueueSend(sampleQueue, &block, 0) != pdTRUE)
    {
        droppedBlocks++;
    }
    #endif

    totalSamples += sampleCount;

    uint32_t now = millis();
    if (now - lastReport >= 1000)
    {
        float rate = totalSamples * 1000.0f / (now - lastReport);

        #if !TELEMETRY_VIEWER
        Serial.println("--------------------------------");
        Serial.printf("DMA Rate     : %.1f samples/sec (target %d)\n", rate, SAMPLE_RATE);
        Serial.printf("Queue Used   : %u / %u\n",
                      (unsigned)(SAMPLE_QUEUE_DEPTH - uxQueueSpacesAvailable(sampleQueue)),
                      (unsigned)SAMPLE_QUEUE_DEPTH);
        Serial.printf("Dropped      : %u blocks\n", (unsigned)droppedBlocks);
        Serial.printf("Free Heap    : %u bytes\n", (unsigned)ESP.getFreeHeap());
        Serial.println("--------------------------------");
        #endif

        totalSamples = 0;
        lastReport = now;
    }
}
