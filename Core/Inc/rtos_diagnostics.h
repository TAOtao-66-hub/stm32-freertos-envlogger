#ifndef RTOS_DIAGNOSTICS_H
#define RTOS_DIAGNOSTICS_H

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f1xx_hal.h"

#define RTOS_DIAGNOSTIC_TASK_COUNT 5U /* Four application tasks and idle. */

typedef struct
{
    uint32_t free_heap_bytes;
    uint32_t minimum_heap_bytes;
    uint32_t minimum_stack_words[RTOS_DIAGNOSTIC_TASK_COUNT];
    uint8_t available[RTOS_DIAGNOSTIC_TASK_COUNT];
} RtosDiagnostics_Snapshot;

/* Bind after all checked xTaskCreate calls, before starting the scheduler.
 * Handles remain valid for the lifetime of these application tasks. */
void RtosDiagnostics_Bind(TaskHandle_t monitor, TaskHandle_t alarm,
                          TaskHandle_t serial, TaskHandle_t storage);
/* Task context only. Stack values are lifetime minima, not current SP space. */
void RtosDiagnostics_Read(RtosDiagnostics_Snapshot *snapshot);
/* Sole UART owner: serialTask. No allocations or scheduler-wide trace needed. */
HAL_StatusTypeDef RtosDiagnostics_Print(void);

#endif
