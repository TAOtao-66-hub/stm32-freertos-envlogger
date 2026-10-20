#include "settings_store.h"
#include "w25qxx.h"
#include "storage_codec.h"
#include <string.h>

#define SETTINGS_MAGIC 0x31544553UL

static uint8_t config_valid(const EnvAlarm_Config *config)
{
    return ((config->temperature_on_x10 >= ENV_ALARM_TEMP_HYSTERESIS_X10) &&
            (config->temperature_on_x10 <= ENV_ALARM_TEMP_SET_MAX_X10) &&
            (config->humidity_on_x10 >= ENV_ALARM_HUM_HYSTERESIS_X10) &&
            (config->humidity_on_x10 <= ENV_ALARM_HUM_SET_MAX_X10) &&
            (config->temperature_off_x10 == config->temperature_on_x10 - ENV_ALARM_TEMP_HYSTERESIS_X10) &&
            (config->humidity_off_x10 == config->humidity_on_x10 - ENV_ALARM_HUM_HYSTERESIS_X10)) ? 1U : 0U;
}

static void decode(const uint8_t *record, EnvAlarm_Config *config)
{
    config->temperature_on_x10 = StorageCodec_Get16(record + 8U);
    config->temperature_off_x10 = StorageCodec_Get16(record + 10U);
    config->humidity_on_x10 = StorageCodec_Get16(record + 12U);
    config->humidity_off_x10 = StorageCodec_Get16(record + 14U);
}

static int latest(uint8_t records[2][32])
{
    EnvAlarm_Config config;
    uint8_t valid[2];
    uint8_t index;
    for (index = 0U; index < 2U; ++index)
    {
        decode(records[index], &config);
        valid[index] = ((StorageCodec_Valid(records[index], SETTINGS_MAGIC) != 0U) &&
                         (config_valid(&config) != 0U)) ? 1U : 0U;
    }
    if (valid[0] == 0U) { return (valid[1] != 0U) ? 1 : -1; }
    if (valid[1] == 0U) { return 0; }
    return ((int32_t)(StorageCodec_Get32(records[1] + 4U) - StorageCodec_Get32(records[0] + 4U)) > 0) ? 1 : 0;
}

SettingsStore_Status SettingsStore_Load(EnvAlarm_Config *config)
{
    uint8_t records[2][32];
    int index;
    if ((config == NULL) || (W25Q_Read(0U, records[0], 32U) == 0U) ||
        (W25Q_Read(FLASH_SECTOR_BYTES, records[1], 32U) == 0U)) { return SETTINGS_STORE_ERROR; }
    index = latest(records);
    if (index < 0) { return SETTINGS_STORE_EMPTY; }
    decode(records[index], config);
    return SETTINGS_STORE_OK;
}

SettingsStore_Status SettingsStore_Save(const EnvAlarm_Config *config)
{
    uint8_t records[2][32];
    uint8_t record[32] = {0U};
    EnvAlarm_Config saved;
    uint32_t address;
    uint32_t sequence = 1U;
    int index;

    if ((config == NULL) || (config_valid(config) == 0U) ||
        (W25Q_Read(0U, records[0], 32U) == 0U) ||
        (W25Q_Read(FLASH_SECTOR_BYTES, records[1], 32U) == 0U)) { return SETTINGS_STORE_ERROR; }
    index = latest(records);
    if (index >= 0)
    {
        decode(records[index], &saved);
        if ((saved.temperature_on_x10 == config->temperature_on_x10) &&
            (saved.temperature_off_x10 == config->temperature_off_x10) &&
            (saved.humidity_on_x10 == config->humidity_on_x10) &&
            (saved.humidity_off_x10 == config->humidity_off_x10)) { return SETTINGS_STORE_UNCHANGED; }
        sequence = StorageCodec_Get32(records[index] + 4U) + 1U;
    }
    address = (index == 0) ? FLASH_SECTOR_BYTES : 0U;
    StorageCodec_Put32(record, SETTINGS_MAGIC);
    StorageCodec_Put32(record + 4U, sequence);
    StorageCodec_Put16(record + 8U, config->temperature_on_x10);
    StorageCodec_Put16(record + 10U, config->temperature_off_x10);
    StorageCodec_Put16(record + 12U, config->humidity_on_x10);
    StorageCodec_Put16(record + 14U, config->humidity_off_x10);
    if ((W25Q_EraseSector(address) == 0U) || (StorageCodec_Write(address, record) == 0U)) { return SETTINGS_STORE_ERROR; }
    return SETTINGS_STORE_OK;
}
