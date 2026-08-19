#ifndef ADC_PROTOCOL_H
#define ADC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

// Protocol Constants
#define ADC_SYNC_WORD        0x55AA55AA  // 32-bit synchronization word
#define ADC_DEFAULT_SAMPLES  1024        // Default sample count per packet frame
#define ADC_BAUDRATE         2000000     // 2 Mbaud UART transfer rate

/**
 * Packed Binary Header Structure (16 bytes total)
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t sync;          // Sync word: 0x55AA55AA
    uint32_t sequence;      // Sequential packet counter (0 to 2^32-1)
    uint16_t sample_count;  // Number of uint16_t ADC samples in payload
    uint32_t timestamp_us;  // ESP32 micros() timestamp at frame read completion
    uint16_t crc16;         // CRC-16 CCITT checksum calculated over payload bytes
} adc_packet_header_t;
#pragma pack(pop)

/**
 * CRC-16 CCITT calculation (Polynomial: 0x1021, Initial: 0xFFFF)
 * Evaluates binary payload integrity.
 */
static inline uint16_t calculate_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

#endif // ADC_PROTOCOL_H
