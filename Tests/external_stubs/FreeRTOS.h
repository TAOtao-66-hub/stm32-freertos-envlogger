#ifndef EXTERNAL_TEST_FREERTOS_H
#define EXTERNAL_TEST_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
typedef int32_t BaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t UBaseType_t;
#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(value) (value)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#endif
