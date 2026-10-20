#ifndef OLED_TEST_FREERTOS_H
#define OLED_TEST_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#endif
