#ifndef SERIAL_TEST_QUEUE_H
#define SERIAL_TEST_QUEUE_H
#include "FreeRTOS.h"
typedef struct
{
    uint8_t bytes[2];
    uint8_t count;
} TestByteQueue;
typedef TestByteQueue *QueueHandle_t;
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks);
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item, BaseType_t *wake);
#endif
