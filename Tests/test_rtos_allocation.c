#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "env_alarm.h"
#include "storage_service.h"
#include "history_buffer.h"
#include "rtos_diagnostics.h"
#include <string.h>
#include <stdio.h>

static char diagnostic_text[1024];
static uint32_t diagnostic_writes;
static uint8_t fail_diagnostic_tx;
HAL_StatusTypeDef SerialPort_WriteString(const char *text)
{
    ++diagnostic_writes;
    if (fail_diagnostic_tx != 0U) { return HAL_TIMEOUT; }
    if (strlen(diagnostic_text) + strlen(text) >= sizeof(diagnostic_text)) { return HAL_ERROR; }
    (void)strcat(diagnostic_text, text);
    return HAL_OK;
}

static void dummy_task(void *argument) { (void)argument; for (;;) {} }
uint32_t SystemCoreClock = 64000000U;
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, uint32_t *size)
{
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
    *tcb=&idle_tcb; *stack=idle_stack; *size=configMINIMAL_STACK_SIZE;
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; (void)name; for (;;) {} }
static uint32_t record_seconds;
void TimeService_Get(TimeService_Snapshot *snapshot)
{
    snapshot->timestamp = 0U;
    snapshot->uptime_seconds = record_seconds;
    snapshot->calibrated = 0U;
}

/* Real heap_4, queues and xTaskCreate; no scheduler/hardware execution. */
uint32_t RunAllocationTests(void)
{
    TaskHandle_t handles[4];
    QueueHandle_t queues[11];
    uint32_t index;
    EnvAlarm_Sample sample = {250U, 600U, 1U};
    History_Sample record;
    HistoryBuffer_Stats stats;
    RtosDiagnostics_Snapshot snapshot;
    void *temporary;
    uint32_t free_before;
    char expected[112];
    queues[0]=xQueueCreate(1U,sizeof(EnvAlarm_Sample));
    queues[1]=xQueueCreate(1U,sizeof(EnvAlarm_State));
    queues[2]=xQueueCreate(SERIAL_RX_QUEUE_LENGTH,sizeof(uint8_t));
    queues[3]=xQueueCreate(1U,sizeof(EnvAlarm_Sample));
    queues[4]=xQueueCreate(1U,sizeof(EnvAlarm_Request));
    queues[5]=xQueueCreate(1U,sizeof(EnvAlarm_Response));
    queues[6]=xQueueCreate(STORAGE_REQUEST_QUEUE_LENGTH,sizeof(Storage_Request));
    queues[7]=xQueueCreate(HISTORY_RECORD_QUEUE_LENGTH,sizeof(History_Sample));
    queues[8]=xQueueCreate(1U,sizeof(Storage_Boot));
    queues[9]=xQueueCreate(1U,sizeof(Storage_SaveReply));
    queues[10]=xQueueCreate(STORAGE_TEXT_QUEUE_LENGTH,sizeof(Storage_TextReply));
    for(index=0U;index<11U;++index) { if(queues[index]==NULL) { return 1U; } }
    if(xTaskCreate(dummy_task,"env",ENV_MONITOR_TASK_STACK_WORDS,NULL,ENV_MONITOR_TASK_PRIORITY,&handles[0])!=pdPASS) { return 2U; }
    if(xTaskCreate(dummy_task,"alarm",ALARM_TASK_STACK_WORDS,NULL,ALARM_TASK_PRIORITY,&handles[1])!=pdPASS) { return 3U; }
    if(xTaskCreate(dummy_task,"serial",SERIAL_TASK_STACK_WORDS,NULL,SERIAL_TASK_PRIORITY,&handles[2])!=pdPASS) { return 4U; }
    if(xTaskCreate(dummy_task,"storage",STORAGE_TASK_STACK_WORDS,NULL,STORAGE_TASK_PRIORITY,&handles[3])!=pdPASS) { return 5U; }
    /* Exercise the actual producer with real FreeRTOS queues, including full
     * handling and copied payloads. No scheduler or peripheral is simulated. */
    HistoryBuffer_Init(queues[7], 0U);
    for (index = 1U; index <= HISTORY_RECORD_QUEUE_LENGTH + 1U; ++index)
    {
        record_seconds = index * 10U;
        sample.temperature_x10 = (uint16_t)(250U + index);
        HistoryBuffer_Poll(index * HISTORY_RECORD_INTERVAL_MS, &sample, 0U);
    }
    HistoryBuffer_GetStats(&stats);
    if ((stats.used != HISTORY_RECORD_QUEUE_LENGTH) || (stats.accepted != HISTORY_RECORD_QUEUE_LENGTH) ||
        (stats.dropped_full != 1U)) { return 6U; }
    for (index = 1U; index <= HISTORY_RECORD_QUEUE_LENGTH; ++index)
    {
        if ((xQueueReceive(queues[7], &record, 0U) != pdPASS) ||
            (record.sample.temperature_x10 != 250U + index) ||
            (record.time.uptime_seconds != index * 10U)) { return 7U; }
    }
    record_seconds = (HISTORY_RECORD_QUEUE_LENGTH + 2U) * 10U;
    HistoryBuffer_Poll((HISTORY_RECORD_QUEUE_LENGTH + 2U) * HISTORY_RECORD_INTERVAL_MS, &sample, 0U);
    HistoryBuffer_GetStats(&stats);
    if ((stats.used != 1U) || (stats.accepted != HISTORY_RECORD_QUEUE_LENGTH + 1U) ||
        (stats.dropped_full != 1U) || (xQueueReceive(queues[7], &record, 0U) != pdPASS) ||
        (record.time.uptime_seconds != record_seconds)) { return 8U; }
    RtosDiagnostics_Bind(handles[0],handles[1],handles[2],handles[3]);
    RtosDiagnostics_Read(&snapshot);
    free_before = snapshot.free_heap_bytes;
    if (free_before != xPortGetFreeHeapSize()) { return 9U; }
    for (index=0U;index<4U;++index)
    {
        if (snapshot.available[index]==0U || snapshot.minimum_stack_words[index]==0U ||
            snapshot.minimum_stack_words[index]>SERIAL_TASK_STACK_WORDS) { return 10U; }
    }
    if (snapshot.available[4]!=0U) { return 11U; } /* Scheduler has not created idle yet. */
    temporary=pvPortMalloc(600U);
    if (temporary==NULL) { return 12U; }
    vPortFree(temporary);
    RtosDiagnostics_Read(&snapshot);
    if (snapshot.free_heap_bytes!=free_before || snapshot.minimum_heap_bytes>=RTOS_HEAP_WARNING_BYTES)
    { return 13U; } /* Free restores current bytes, but lifetime minimum stays low. */
    (void)snprintf(expected,sizeof(expected),"RTOS heap_total=%luB free=%luB min=%luB state=LOW",
                   (unsigned long)configTOTAL_HEAP_SIZE,(unsigned long)free_before,
                   (unsigned long)snapshot.minimum_heap_bytes);
    if (RtosDiagnostics_Print()!=HAL_OK || diagnostic_writes!=6U ||
        strstr(diagnostic_text,expected)==NULL ||
        strstr(diagnostic_text,"TASK envMonitorTask total=256W min_free=")==NULL ||
        strstr(diagnostic_text,"TASK storageTask total=384W min_free=")==NULL ||
        strstr(diagnostic_text,"TASK idle state=UNAVAILABLE")==NULL) { return 14U; }
    fail_diagnostic_tx=1U; diagnostic_writes=0U;
    if (RtosDiagnostics_Print()!=HAL_ERROR || diagnostic_writes!=1U) { return 15U; }
    RtosDiagnostics_Read(NULL);
    return (uint32_t)xPortGetFreeHeapSize();
}
