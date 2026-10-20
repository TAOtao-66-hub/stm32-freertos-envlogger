#ifndef SERIAL_TEST_FREERTOS_H
#define SERIAL_TEST_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
typedef int32_t BaseType_t;
typedef uint32_t TickType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define pdFAIL 0
void test_yield_from_isr(BaseType_t wake);
#define portYIELD_FROM_ISR(wake) test_yield_from_isr(wake)
#endif
