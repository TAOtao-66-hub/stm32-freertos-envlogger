#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include "env_alarm.h"

typedef enum
{
    SETTINGS_STORE_OK = 0,
    SETTINGS_STORE_UNCHANGED,
    SETTINGS_STORE_EMPTY,
    SETTINGS_STORE_ERROR
} SettingsStore_Status;

/* storageTask owns all external W25Q access. Sectors 0/1 hold alternating
 * committed CRC records. No internal MCU Flash writes and no sensor mutex. */
SettingsStore_Status SettingsStore_Load(EnvAlarm_Config *config);
SettingsStore_Status SettingsStore_Save(const EnvAlarm_Config *config);

#endif /* SETTINGS_STORE_H */
