#ifndef HISTORY_BUFFER_H
#define HISTORY_BUFFER_H

#include "FreeRTOS.h"
#include "queue.h"
#include "history_store.h"

typedef struct
{
    uint32_t accepted;
    uint32_t dropped_full;
    uint32_t missed_periods;
    uint32_t peak;
    uint32_t used;
    uint32_t discarded_clear; /* Queued samples intentionally removed after clear. */
    uint32_t paused_periods;  /* Record periods deliberately skipped during clear. */
    uint8_t paused;
} HistoryBuffer_Stats;

/* Sole producer: alarmTask. Queue items are copied History_Sample values.
 * Call once after boot settings are received; Storage owns dequeue/write. */
void HistoryBuffer_Init(QueueHandle_t queue, uint32_t now_ms);
/* Capture one latest-sample snapshot at each record period, independently
 * of Flash progress. Its time is the record creation time, never write time.
 * NULL sample means no measurement is available. A full FIFO drops the NEW
 * record without waiting, preserving older queued records. */
void HistoryBuffer_Poll(uint32_t now_ms, const EnvAlarm_Sample *sample, uint8_t alarm_flags);
/* Safe RAM-only snapshot for serial/status. */
void HistoryBuffer_GetStats(HistoryBuffer_Stats *stats);
/* Storage owner only: pause producer, commit clear, discard old queued samples
 * on success, then resume. Failure resumes without discarding the old FIFO. */
void HistoryBuffer_SetPaused(uint8_t paused);
uint32_t HistoryBuffer_DiscardPending(void);

#endif
