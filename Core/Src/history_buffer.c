#include "history_buffer.h"
#include "project_config.h"
#include "task.h"
#include <string.h>

typedef struct
{
    QueueHandle_t queue;
    uint32_t last_period_ms;
    HistoryBuffer_Stats stats;
} HistoryBuffer_Context;

static HistoryBuffer_Context s_buffer;

void HistoryBuffer_Init(QueueHandle_t queue, uint32_t now_ms)
{
    taskENTER_CRITICAL();
    (void)memset(&s_buffer, 0, sizeof(s_buffer));
    s_buffer.queue = queue;
    s_buffer.last_period_ms = now_ms;
    taskEXIT_CRITICAL();
}

void HistoryBuffer_Poll(uint32_t now_ms, const EnvAlarm_Sample *sample, uint8_t alarm_flags)
{
    History_Sample record = {0U};
    uint32_t periods;
    uint32_t depth;
    BaseType_t result;

    if (s_buffer.queue == NULL) { return; }
    periods = (uint32_t)(now_ms - s_buffer.last_period_ms) / HISTORY_RECORD_INTERVAL_MS;
    if (periods == 0U) { return; }
    /* Preserve phase. Do not manufacture old records from today's sample
     * when the producer itself was delayed across several record periods. */
    s_buffer.last_period_ms += periods * HISTORY_RECORD_INTERVAL_MS;
    taskENTER_CRITICAL();
    if (s_buffer.stats.paused != 0U)
    {
        s_buffer.stats.paused_periods += periods;
        taskEXIT_CRITICAL();
        return;
    }
    taskEXIT_CRITICAL();
    if (sample != NULL) { record.sample = *sample; }
    record.alarm_flags = alarm_flags;
    if (record.sample.valid == 0U)
    {
        record.sample.temperature_x10 = 0U;
        record.sample.humidity_x10 = 0U;
        record.alarm_flags |= ENV_ALARM_FLAG_SENSOR;
    }
    TimeService_Get(&record.time);
    result = xQueueSend(s_buffer.queue, &record, 0U);
    depth = (uint32_t)uxQueueMessagesWaiting(s_buffer.queue);
    taskENTER_CRITICAL();
    s_buffer.stats.missed_periods += periods - 1U;
    if (result == pdPASS)
    {
        ++s_buffer.stats.accepted;
        if (depth > s_buffer.stats.peak) { s_buffer.stats.peak = depth; }
    }
    else { ++s_buffer.stats.dropped_full; }
    taskEXIT_CRITICAL();
}

void HistoryBuffer_GetStats(HistoryBuffer_Stats *stats)
{
    if (stats == NULL) { return; }
    taskENTER_CRITICAL();
    *stats = s_buffer.stats;
    stats->used = (s_buffer.queue != NULL) ? (uint32_t)uxQueueMessagesWaiting(s_buffer.queue) : 0U;
    taskEXIT_CRITICAL();
}

void HistoryBuffer_SetPaused(uint8_t paused)
{
    taskENTER_CRITICAL();
    s_buffer.stats.paused = (paused != 0U) ? 1U : 0U;
    taskEXIT_CRITICAL();
}

uint32_t HistoryBuffer_DiscardPending(void)
{
    History_Sample sample;
    uint32_t removed = 0U;
    /* Producer is paused and Storage is the sole consumer. Bounded by FIFO
     * capacity; never reset counters or the sampling/record timing phase. */
    if ((s_buffer.stats.paused == 0U) || (s_buffer.queue == NULL)) { return 0U; }
    while ((removed < HISTORY_RECORD_QUEUE_LENGTH) &&
           (xQueueReceive(s_buffer.queue, &sample, 0U) == pdPASS)) { ++removed; }
    taskENTER_CRITICAL();
    s_buffer.stats.discarded_clear += removed;
    taskEXIT_CRITICAL();
    return removed;
}
