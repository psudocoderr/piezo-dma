#include <Arduino.h>
#include <string.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <driver/spi_master.h>
#include "secrets.h"

// ------------------------------------------------------------
// Transport
// ------------------------------------------------------------
#define ENABLE_UART_STREAM 1
#define ENABLE_UDP_STREAM  1

#define BAUDRATE 2000000

// ------------------------------------------------------------
// SPI configuration
// ------------------------------------------------------------
// 80 kHz * 24 bits = 1.92 Mbps. 
// Set clock to 4 MHz to provide enough overhead margin.
#define SPI_FREQ_HZ (4 * 1000 * 1000) 

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23 
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

// 3 bytes per sample. 
// 340 samples * 3 bytes = 1020 bytes per DMA frame
#define BYTES_PER_SAMPLE  3
#define SAMPLES_PER_FRAME 340
#define SPI_FRAME_SIZE    (SAMPLES_PER_FRAME * BYTES_PER_SAMPLE)

// One sync byte + 1020 bytes SPI data
#define TX_FRAME_SIZE (1 + SPI_FRAME_SIZE)

// ------------------------------------------------------------
// Globals
// ------------------------------------------------------------
spi_device_handle_t spi_handle;

// DMA Double buffering
DMA_ATTR uint8_t rx_buf[2][SPI_FRAME_SIZE];
spi_transaction_t trans[2];

uint8_t txBuffer[TX_FRAME_SIZE];

uint32_t lastWifiCheckMs = 0;

WiFiUDP udp;
IPAddress udpTarget;


// ------------------------------------------------------------
// WiFi
// ------------------------------------------------------------
void setupWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to WiFi");
    uint32_t start = millis();

    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000)
    {
        delay(250);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("WiFi connected, IP=");
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println("WiFi connection timeout");
    }

    udpTarget.fromString(UDP_TARGET_IP);
    udp.begin(UDP_LOCAL_PORT);
}


// ------------------------------------------------------------
// SPI DMA Initialization
// ------------------------------------------------------------
void setupSpiDma()
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_FRAME_SIZE
    };

    spi_device_interface_config_t devcfg = {
        .mode = 0,                         
        .clock_speed_hz = SPI_FREQ_HZ,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 2,                   
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle));

    memset(trans, 0, sizeof(trans));

    // Queue both buffers
    for (int i = 0; i < 2; i++) {
        trans[i].length = SPI_FRAME_SIZE * 8; 
        trans[i].rx_buffer = rx_buf[i];
        trans[i].user = (void*)i;             

        ESP_ERROR_CHECK(spi_device_queue_trans(spi_handle, &trans[i], portMAX_DELAY));
    }
}


// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------
void setup()
{
    Serial.begin(BAUDRATE);
    delay(500);

#if ENABLE_UDP_STREAM
    setupWiFi();
#endif

    setupSpiDma();
    Serial.println("SPI DMA streaming (3-byte samples) started");
}


// ------------------------------------------------------------
// Main loop
// ------------------------------------------------------------
void loop()
{
#if ENABLE_UDP_STREAM
    uint32_t nowMs = millis();
    if (WiFi.status() != WL_CONNECTED && (nowMs - lastWifiCheckMs) >= WIFI_RECONNECT_INTERVAL_MS)
    {
        lastWifiCheckMs = nowMs;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
#endif

    // Wait for DMA frame
    spi_transaction_t *p_trans;
    esp_err_t ret = spi_device_get_trans_result(spi_handle, &p_trans, portMAX_DELAY);

    if (ret != ESP_OK) return;

    int buf_idx = (int)p_trans->user;
    uint8_t *frameBuffer = rx_buf[buf_idx];

    // --------------------------------------------------------
    // Construct Telemetry Frame
    // --------------------------------------------------------
    txBuffer[0] = 0xAA; // Sync byte

    // Since the external ADC is already outputting exactly 3 bytes per sample, 
    // we can copy the entire 1020-byte block directly into the TX buffer in one shot
    // rather than looping through it byte-by-byte. This is significantly faster.
    memcpy(&txBuffer[1], frameBuffer, SPI_FRAME_SIZE);

    size_t totalLen = 1 + SPI_FRAME_SIZE;

#if ENABLE_UART_STREAM
    Serial.write(txBuffer, totalLen);
#endif

#if ENABLE_UDP_STREAM
    if (WiFi.status() == WL_CONNECTED)
    {
        udp.beginPacket(udpTarget, UDP_TARGET_PORT);
        udp.write(txBuffer, totalLen);
        udp.endPacket();
    }
#endif

    // Requeue the DMA transaction immediately
    ESP_ERROR_CHECK(spi_device_queue_trans(spi_handle, p_trans, portMAX_DELAY));
}