#ifndef STORAGE_CODEC_H
#define STORAGE_CODEC_H
#include <stdint.h>

/* Fixed 32-byte, little-endian records. CRC covers bytes 0..23;
 * bytes 28..31 are the last-programmed commit marker. */
uint16_t StorageCodec_Get16(const uint8_t *data);
uint32_t StorageCodec_Get32(const uint8_t *data);
void StorageCodec_Put16(uint8_t *data, uint16_t value);
void StorageCodec_Put32(uint8_t *data, uint32_t value);
uint32_t StorageCodec_Crc32(const uint8_t *data, uint32_t length);
uint8_t StorageCodec_Valid(const uint8_t *record, uint32_t magic);
uint8_t StorageCodec_Blank(const uint8_t *data, uint32_t length);
uint8_t StorageCodec_Write(uint32_t address, uint8_t *record);
#endif
