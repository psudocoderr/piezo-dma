/*
 * ESP32 Piezo ADC continuous DMA -> binary UDP
 *
 * Target: original ESP32 DevKit V1
 * Input:  GPIO34 / ADC1_CHANNEL_6
 * Rate:   80,000 samples/second by default
 *
 * This sketch uses the Arduino-ESP32 WiFi API and the ESP-IDF continuous ADC
 * driver exposed by the Arduino core. The original ESP32 internal ADC uses
 * its I2S-backed DMA path; it is not the general-purpose SPI DMA engine.
 *
 * Arduino-ESP32 3.x is recommended.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_adc/adc_continuous.h"
#include "esp_err.h"
#include "secrets.h"


IPAddress UDP_DESTINATION(192, 168, 0, 175);
constexpr uint16_t UDP_PORT = 5005;

// GPIO34 = ADC1_CHANNEL_6 on the original ESP32 DevKit V1.
constexpr adc_unit_t ADC_UNIT_USED = ADC_UNIT_1;
constexpr adc_channel_t ADC_CHANNEL_USED = ADC_CHANNEL_6;
constexpr adc_atten_t ADC_ATTENUATION = ADC_ATTEN_DB_11;
constexpr adc_bitwidth_t ADC_BIT_WIDTH = ADC_BITWIDTH_12;

constexpr uint32_t SAMPLE_RATE_HZ = 80000;
constexpr uint16_t SAMPLES_PER_PACKET = 240;

// DMA storage must be larger than one UDP packet to absorb short Wi-Fi stalls.
constexpr uint32_t DMA_STORE_BYTES = 8192;
constexpr uint32_t DMA_FRAME_BYTES = 1024;

// ---------------------------------------------------------------------------
// Binary packet format
// ---------------------------------------------------------------------------
//
// offset  size  field
// 0       2     sync bytes: 0xA5 0x5A
// 2       1     protocol version: 1
// 3       1     flags: 0
// 4       4     packet sequence, uint32 little-endian
// 8       4     sample rate in Hz, uint32 little-endian
// 12      4     first sample index, uint32 little-endian
// 16      2     sample count, uint16 little-endian
// 18      2     payload byte count, uint16 little-endian
// 20      N     ADC samples, uint16 little-endian
// 20+N    2     CRC-16/CCITT-FALSE

constexpr uint8_t PACKET_SYNC_0 = 0xA5;
constexpr uint8_t PACKET_SYNC_1 = 0x5A;
constexpr uint8_t PACKET_VERSION = 1;
constexpr size_t PACKET_HEADER_BYTES = 20;
constexpr size_t PACKET_CRC_BYTES = 2;
constexpr size_t PACKET_MAX_BYTES =
    PACKET_HEADER_BYTES + (SAMPLES_PER_PACKET * sizeof(uint16_t)) +
    PACKET_CRC_BYTES;

static_assert(PACKET_MAX_BYTES < 1472, "Keep packets below the normal MTU");

WiFiUDP udp;
adc_continuous_handle_t adcHandle = nullptr;

uint32_t packetSequence = 0;
uint32_t totalSamples = 0;
uint32_t udpErrors = 0;
uint32_t dmaErrors = 0;

uint16_t crc16CcittFalse(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;

  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

void putU16Le(uint8_t *destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFF);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void putU32Le(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFF);
  destination[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  destination[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  destination[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

size_t makePacket(
    uint8_t *packet,
    const uint16_t *samples,
    uint16_t sampleCount,
    uint32_t firstSampleIndex) {
  const uint16_t payloadBytes =
      static_cast<uint16_t>(sampleCount * sizeof(uint16_t));

  packet[0] = PACKET_SYNC_0;
  packet[1] = PACKET_SYNC_1;
  packet[2] = PACKET_VERSION;
  packet[3] = 0;
  putU32Le(&packet[4], packetSequence);
  putU32Le(&packet[8], SAMPLE_RATE_HZ);
  putU32Le(&packet[12], firstSampleIndex);
  putU16Le(&packet[16], sampleCount);
  putU16Le(&packet[18], payloadBytes);

  for (uint16_t i = 0; i < sampleCount; ++i) {
    putU16Le(
        &packet[PACKET_HEADER_BYTES + (i * sizeof(uint16_t))], samples[i]);
  }

  const size_t crcOffset = PACKET_HEADER_BYTES + payloadBytes;
  putU16Le(&packet[crcOffset], crc16CcittFalse(packet, crcOffset));
  return crcOffset + PACKET_CRC_BYTES;
}

bool sendPacket(
    const uint16_t *samples,
    uint16_t sampleCount,
    uint32_t firstSampleIndex) {
  uint8_t packet[PACKET_MAX_BYTES];
  const size_t packetLength =
      makePacket(packet, samples, sampleCount, firstSampleIndex);

  if (!udp.beginPacket(UDP_DESTINATION, UDP_PORT)) {
    ++udpErrors;
    return false;
  }

  const size_t written = udp.write(packet, packetLength);
  const int result = udp.endPacket();
  if (written != packetLength || result != 1) {
    ++udpErrors;
    return false;
  }

  ++packetSequence;
  return true;
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (++attempts >= 60) {
      Serial.println("\nWi-Fi connection failed; restarting.");
      ESP.restart();
    }
  }

  Serial.println();
  Serial.print("Wi-Fi address: ");
  Serial.println(WiFi.localIP());

  // The local port is not part of the receiver protocol. It only needs to be
  // valid so WiFiUDP can allocate its socket.
  if (!udp.begin(49152)) {
    Serial.println("UDP socket setup failed; restarting.");
    ESP.restart();
  }

  Serial.print("UDP destination: ");
  Serial.print(UDP_DESTINATION);
  Serial.print(':');
  Serial.println(UDP_PORT);
}

void startAdcContinuousDma() {
  const adc_continuous_handle_cfg_t handleConfig = {
      .max_store_buf_size = DMA_STORE_BYTES,
      .conv_frame_size = DMA_FRAME_BYTES,
  };
  ESP_ERROR_CHECK(adc_continuous_new_handle(&handleConfig, &adcHandle));

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTENUATION;
  pattern.channel = ADC_CHANNEL_USED;
  pattern.unit = ADC_UNIT_USED;
  pattern.bit_width = ADC_BIT_WIDTH;

  adc_continuous_config_t adcConfig = {};
  adcConfig.pattern_num = 1;
  adcConfig.adc_pattern = &pattern;
  adcConfig.sample_freq_hz = SAMPLE_RATE_HZ;
  adcConfig.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  adcConfig.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;

  ESP_ERROR_CHECK(adc_continuous_config(adcHandle, &adcConfig));
  ESP_ERROR_CHECK(adc_continuous_start(adcHandle));

  Serial.printf(
      "ADC1 channel %d / GPIO34 sampling at %lu Hz via continuous DMA\n",
      static_cast<int>(ADC_CHANNEL_USED),
      static_cast<unsigned long>(SAMPLE_RATE_HZ));
}

void adcStreamTask(void *parameter) {
  (void)parameter;

  uint8_t dmaFrame[DMA_FRAME_BYTES];
  uint16_t samples[SAMPLES_PER_PACKET];
  uint16_t samplesInPacket = 0;
  uint32_t lastReportMs = millis();

  while (true) {
    uint32_t bytesRead = 0;
    const esp_err_t result = adc_continuous_read(
        adcHandle,
        dmaFrame,
        sizeof(dmaFrame),
        &bytesRead,
        1000);

    if (result == ESP_ERR_TIMEOUT) {
      Serial.println("ADC DMA read timeout");
      continue;
    }
    if (result == ESP_ERR_INVALID_STATE) {
      ++dmaErrors;
      Serial.printf("ADC DMA error (%lu total)\n",
                    static_cast<unsigned long>(dmaErrors));
      continue;
    }
    ESP_ERROR_CHECK(result);

    for (uint32_t offset = 0;
         offset + sizeof(adc_digi_output_data_t) <= bytesRead;
         offset += sizeof(adc_digi_output_data_t)) {
      const adc_digi_output_data_t *sample =
          reinterpret_cast<const adc_digi_output_data_t *>(&dmaFrame[offset]);

      if (sample->type1.channel != ADC_CHANNEL_USED) {
        continue;
      }

      samples[samplesInPacket++] = sample->type1.data & 0x0FFF;
      ++totalSamples;

      if (samplesInPacket == SAMPLES_PER_PACKET) {
        sendPacket(
            samples,
            samplesInPacket,
            totalSamples - samplesInPacket);
        samplesInPacket = 0;
      }
    }

    const uint32_t now = millis();
    if (now - lastReportMs >= 5000) {
      Serial.printf(
          "stream: samples=%lu packets=%lu udp_errors=%lu dma_errors=%lu\n",
          static_cast<unsigned long>(totalSamples),
          static_cast<unsigned long>(packetSequence),
          static_cast<unsigned long>(udpErrors),
          static_cast<unsigned long>(dmaErrors));
      lastReportMs = now;
    }
  }
}

void setup() {
  Serial.begin(2000000);
  delay(500);
  Serial.println("\nESP32 piezo DMA UDP streamer");

  connectToWiFi();
  startAdcContinuousDma();

  xTaskCreatePinnedToCore(
      adcStreamTask,
      "adc_stream",
      4096,
      nullptr,
      configMAX_PRIORITIES - 2,
      nullptr,
      1);
}

void loop() {
  // Sampling and UDP transmission run in the pinned FreeRTOS task.
  delay(100);
}