#include "rtos_diagnostics.h"
#include "serial_port.h"
#include "project_config.h"
#include <stdio.h>
#include <string.h>

#if (INCLUDE_uxTaskGetStackHighWaterMark != 1) || (INCLUDE_xTaskGetIdleTaskHandle != 1) || (INCLUDE_xTaskGetSchedulerState != 1)
#error RTOS diagnostics require stack watermark, idle handle and scheduler state APIs.
#endif

typedef struct
{
    const char *name;
    uint32_t stack_words;
} RtosDiagnostics_Task;

static const RtosDiagnostics_Task s_task_info[RTOS_DIAGNOSTIC_TASK_COUNT] =
{
    {"envMonitorTask", ENV_MONITOR_TASK_STACK_WORDS},
    {"alarmTask", ALARM_TASK_STACK_WORDS},
    {"serialTask", SERIAL_TASK_STACK_WORDS},
    {"storageTask", STORAGE_TASK_STACK_WORDS},
    {"idle", configMINIMAL_STACK_SIZE}
};
static TaskHandle_t s_handles[RTOS_DIAGNOSTIC_TASK_COUNT];

void RtosDiagnostics_Bind(TaskHandle_t monitor, TaskHandle_t alarm,
                          TaskHandle_t serial, TaskHandle_t storage)
{
    s_handles[0] = monitor;
    s_handles[1] = alarm;
    s_handles[2] = serial;
    s_handles[3] = storage;
}

void RtosDiagnostics_Read(RtosDiagnostics_Snapshot *snapshot)
{
    uint32_t index;
    if (snapshot == NULL) { return; }
    (void)memset(snapshot, 0, sizeof(*snapshot));
    snapshot->free_heap_bytes = (uint32_t)xPortGetFreeHeapSize();
    snapshot->minimum_heap_bytes = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    s_handles[4] = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) ? xTaskGetIdleTaskHandle() : NULL;
    for (index = 0U; index < RTOS_DIAGNOSTIC_TASK_COUNT; ++index)
    {
        /* NULL would mean 'current task' to the kernel; never use it here. */
        if (s_handles[index] != NULL)
        {
            snapshot->minimum_stack_words[index] = (uint32_t)uxTaskGetStackHighWaterMark(s_handles[index]);
            snapshot->available[index] = 1U;
        }
    }
}

HAL_StatusTypeDef RtosDiagnostics_Print(void)
{
    RtosDiagnostics_Snapshot snapshot;
    char text[112];
    uint32_t index;
    uint32_t stack_bytes;
    RtosDiagnostics_Read(&snapshot);
    (void)snprintf(text, sizeof(text), "RTOS heap_total=%luB free=%luB min=%luB state=%s\r\n",
                   (unsigned long)configTOTAL_HEAP_SIZE, (unsigned long)snapshot.free_heap_bytes,
                   (unsigned long)snapshot.minimum_heap_bytes,
                   (snapshot.minimum_heap_bytes < RTOS_HEAP_WARNING_BYTES) ? "LOW" : "OK");
    if (SerialPort_WriteString(text) != HAL_OK) { return HAL_ERROR; }
    for (index = 0U; index < RTOS_DIAGNOSTIC_TASK_COUNT; ++index)
    {
        stack_bytes = snapshot.minimum_stack_words[index] * sizeof(StackType_t);
        if (snapshot.available[index] == 0U)
        {
            (void)snprintf(text, sizeof(text), "TASK %s state=UNAVAILABLE\r\n", s_task_info[index].name);
        }
        else
        {
            (void)snprintf(text, sizeof(text), "TASK %s total=%luW min_free=%luW/%luB state=%s\r\n",
                           s_task_info[index].name, (unsigned long)s_task_info[index].stack_words,
                           (unsigned long)snapshot.minimum_stack_words[index], (unsigned long)stack_bytes,
                           (stack_bytes < RTOS_STACK_WARNING_BYTES) ? "LOW" : "OK");
        }
        if (SerialPort_WriteString(text) != HAL_OK) { return HAL_ERROR; }
    }
    return HAL_OK;
}
