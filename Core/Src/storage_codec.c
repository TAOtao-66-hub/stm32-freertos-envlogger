#include "storage_codec.h"
#include "w25qxx.h"
#include <string.h>

#define RECORD_COMMIT 0xA55AC33CUL

uint16_t StorageCodec_Get16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}
uint32_t StorageCodec_Get32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}
void StorageCodec_Put16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
}
void StorageCodec_Put32(uint8_t *data, uint32_t value)
{
    StorageCodec_Put16(data, (uint16_t)value);
    StorageCodec_Put16(data + 2U, (uint16_t)(value >> 16U));
}
uint32_t StorageCodec_Crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint8_t bit;
    while (length-- > 0U)
    {
        crc ^= *data++;
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ (((crc & 1U) != 0U) ? 0xEDB88320UL : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}
uint8_t StorageCodec_Valid(const uint8_t *record, uint32_t magic)
{
    return ((StorageCodec_Get32(record) == magic) &&
            (StorageCodec_Get32(record + 24U) == StorageCodec_Crc32(record, 24U)) &&
            (StorageCodec_Get32(record + 28U) == RECORD_COMMIT)) ? 1U : 0U;
}
uint8_t StorageCodec_Blank(const uint8_t *data, uint32_t length)
{
    while (length-- > 0U) { if (*data++ != 0xFFU) { return 0U; } }
    return 1U;
}
uint8_t StorageCodec_Write(uint32_t address, uint8_t *record)
{
    uint8_t check[32];
    StorageCodec_Put32(record + 24U, StorageCodec_Crc32(record, 24U));
    StorageCodec_Put32(record + 28U, RECORD_COMMIT);
    if ((W25Q_Program(address, record, 28U) == 0U) ||
        (W25Q_Read(address, check, 28U) == 0U) ||
        (memcmp(record, check, 28U) != 0)) { return 0U; }
    if ((W25Q_Program(address + 28U, record + 28U, 4U) == 0U) ||
        (W25Q_Read(address, check, 32U) == 0U)) { return 0U; }
    return (memcmp(record, check, 32U) == 0) ? 1U : 0U;
}
