#ifndef EXTERNAL_TEST_QUEUE_H
#define EXTERNAL_TEST_QUEUE_H
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
#endif
