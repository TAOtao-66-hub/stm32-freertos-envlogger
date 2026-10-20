#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H
#include "FreeRTOS.h"
#include "queue.h"
#include "history_store.h"
#include "settings_store.h"

typedef enum
{
    STORAGE_REQUEST_SAVE = 0,
    STORAGE_REQUEST_INFO,
    STORAGE_REQUEST_STATUS,
    STORAGE_REQUEST_READ,
    STORAGE_REQUEST_TIME_SET,
    STORAGE_REQUEST_PAGE,
    STORAGE_REQUEST_EXPORT,
    STORAGE_REQUEST_CANCEL,
    STORAGE_REQUEST_CLEAR
} Storage_RequestKind;
typedef struct
{
    Storage_RequestKind kind;
    uint32_t request_id;
    EnvAlarm_Config config;
    uint16_t count;
    uint32_t timestamp; /* TIME_SET only; validated before enqueueing. */
    uint32_t before_sequence; /* PAGE only: exclusive sequence cursor, zero=newest. */
} Storage_Request;
typedef struct
{
    SettingsStore_Status status;
    EnvAlarm_Config config;
} Storage_Boot;
typedef struct
{
    uint32_t request_id;
    SettingsStore_Status status;
} Storage_SaveReply;
typedef struct
{
    uint32_t request_id;
    uint8_t final; /* 0=data, 1=final, 2=internal progress (do not print). */
    char text[112];
} Storage_TextReply;

/* Only storageTask calls these service functions and all W25Q operations.
 * sample is a FIFO of already timestamped records produced by alarmTask. */
void StorageService_Init(QueueHandle_t request, QueueHandle_t sample,
                         QueueHandle_t boot, QueueHandle_t saved, QueueHandle_t text);
void StorageService_Poll(void);
#endif
