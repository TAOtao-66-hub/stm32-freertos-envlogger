#include "history_transfer.h"
#include "project_config.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

typedef enum
{
    TRANSFER_IDLE = 0,
    TRANSFER_BEGIN,
    TRANSFER_COLUMNS,
    TRANSFER_ROWS,
    TRANSFER_END
} Transfer_Phase;

typedef struct
{
    QueueHandle_t replies;
    History_Reader reader;
    Storage_TextReply pending;
    Transfer_Phase phase;
    uint32_t rows;
    uint32_t last_sequence;
    uint32_t last_progress_ms;
    uint16_t limit;
    uint8_t csv;
    uint8_t pending_valid;
    uint8_t pending_row;
    uint8_t cancelled;
    uint8_t more;
    const char *result;
} Transfer_Context;

static Transfer_Context s_transfer;

uint8_t HistoryTransfer_IsActive(void) { return (s_transfer.phase != TRANSFER_IDLE) ? 1U : 0U; }

void HistoryTransfer_Init(QueueHandle_t replies)
{
    (void)memset(&s_transfer, 0, sizeof(s_transfer));
    s_transfer.replies = replies;
}

uint8_t HistoryTransfer_Begin(const Storage_Request *request)
{
    QueueHandle_t replies = s_transfer.replies;
    /* serialTask serializes public requests; cancellation is queued first. */
    if (s_transfer.phase != TRANSFER_IDLE) { return 0U; }
    (void)memset(&s_transfer, 0, sizeof(s_transfer));
    s_transfer.replies = replies;
    s_transfer.pending.request_id = request->request_id;
    s_transfer.csv = (request->kind == STORAGE_REQUEST_EXPORT) ? 1U : 0U;
    s_transfer.limit = request->count;
    s_transfer.last_progress_ms = HAL_GetTick();
    s_transfer.result = "OK";
    s_transfer.phase = TRANSFER_BEGIN;
    if (HistoryStore_BeginRead(&s_transfer.reader, request->before_sequence) == 0U)
    {
        s_transfer.result = "NOT_READY";
    }
    return 1U;
}

void HistoryTransfer_Cancel(uint32_t request_id)
{
    if ((s_transfer.phase != TRANSFER_IDLE) &&
        (s_transfer.pending.request_id == request_id)) { s_transfer.cancelled = 1U; }
}

static void prepare_end(void)
{
    s_transfer.phase = TRANSFER_END;
    s_transfer.pending.final = 1U;
    if (s_transfer.csv != 0U)
    {
        (void)snprintf(s_transfer.pending.text, sizeof(s_transfer.pending.text),
                       "CSV_END rows=%lu skipped=%lu state=%s\r\n",
                       (unsigned long)s_transfer.rows,
                       (unsigned long)s_transfer.reader.skipped_invalid, s_transfer.result);
    }
    else
    {
        (void)snprintf(s_transfer.pending.text, sizeof(s_transfer.pending.text),
                       "PAGE_END count=%lu next=%lu skipped=%lu state=%s\r\n",
                       (unsigned long)s_transfer.rows,
                       (unsigned long)((s_transfer.more != 0U) ? s_transfer.last_sequence : 0U),
                       (unsigned long)s_transfer.reader.skipped_invalid, s_transfer.result);
    }
    s_transfer.pending_valid = 1U;
}

static void prepare_row(const History_Record *record)
{
    char date[20];
    const char *source;
    source = (record->time_source == HISTORY_TIME_RTC) ? "rtc" :
             ((record->time_source == HISTORY_TIME_ESTIMATED) ? "estimated" : "unknown");
    if (record->time_source == HISTORY_TIME_UNKNOWN) { (void)strcpy(date, "UNCALIBRATED"); }
    else { TimeService_Format(record->display_timestamp, date); }
    (void)snprintf(s_transfer.pending.text, sizeof(s_transfer.pending.text),
                   (s_transfer.csv != 0U) ? "%lu,%lu,%lu,%s,%u.%u,%u.%u,%u,%u,%s\r\n" :
                   "LOG %lu,%lu,%lu,%s,%u.%u,%u.%u,valid=%u,alarm=%u,time=%s\r\n",
                   (unsigned long)record->sequence, (unsigned long)record->session,
                   (unsigned long)record->value.time.uptime_seconds, date,
                   (unsigned int)(record->value.sample.temperature_x10 / 10U),
                   (unsigned int)(record->value.sample.temperature_x10 % 10U),
                   (unsigned int)(record->value.sample.humidity_x10 / 10U),
                   (unsigned int)(record->value.sample.humidity_x10 % 10U),
                   (unsigned int)record->value.sample.valid,
                   (unsigned int)record->value.alarm_flags, source);
    s_transfer.last_sequence = record->sequence;
    s_transfer.pending_row = 1U;
    s_transfer.pending_valid = 1U;
}

void HistoryTransfer_Poll(void)
{
    History_Record record;
    History_ReadStatus status;
    if (s_transfer.phase == TRANSFER_IDLE) { return; }
    if (s_transfer.pending_valid != 0U)
    {
        if (xQueueSend(s_transfer.replies, &s_transfer.pending, 0U) == pdPASS)
        {
            if (s_transfer.pending_row != 0U) { ++s_transfer.rows; }
            if (s_transfer.pending.final == 1U) { s_transfer.phase = TRANSFER_IDLE; }
            s_transfer.pending_valid = s_transfer.pending_row = 0U;
            s_transfer.last_progress_ms = HAL_GetTick();
        }
        return;
    }
    s_transfer.pending.final = 0U;
    if (s_transfer.phase == TRANSFER_BEGIN)
    {
        (void)snprintf(s_transfer.pending.text, sizeof(s_transfer.pending.text),
                       (s_transfer.csv != 0U) ? "CSV_BEGIN snapshot=%lu order=NEWEST_FIRST\r\n" :
                       "PAGE_BEGIN snapshot=%lu order=NEWEST_FIRST\r\n",
                       (unsigned long)s_transfer.reader.snapshot_sequence);
        s_transfer.pending_valid = 1U;
        s_transfer.phase = (s_transfer.csv != 0U) ? TRANSFER_COLUMNS : TRANSFER_ROWS;
        return;
    }
    if (s_transfer.phase == TRANSFER_COLUMNS)
    {
        (void)strcpy(s_transfer.pending.text,
                     "sequence,session,uptime_s,beijingtime,temp_c,humidity_pct,valid,alarm,time_source\r\n");
        s_transfer.pending_valid = 1U;
        s_transfer.phase = TRANSFER_ROWS;
        return;
    }
    if (s_transfer.cancelled != 0U) { s_transfer.result = "CANCELLED"; }
    if (strcmp(s_transfer.result, "OK") != 0) { prepare_end(); return; }
    status = HistoryStore_ReadNext(&s_transfer.reader, &record, HISTORY_SCAN_BUDGET_SLOTS);
    if (status == HISTORY_READ_OK)
    {
        /* One-record lookahead tells the caller whether another page exists. */
        if ((s_transfer.csv == 0U) && (s_transfer.rows >= s_transfer.limit))
        {
            s_transfer.more = 1U;
            prepare_end();
        }
        else { prepare_row(&record); }
    }
    else if (status == HISTORY_READ_EMPTY) { prepare_end(); }
    else if (status != HISTORY_READ_PENDING)
    {
        s_transfer.result = (status == HISTORY_READ_OVERRUN) ? "OVERRUN" : "READ_IO";
        prepare_end();
    }
    else if ((uint32_t)(HAL_GetTick() - s_transfer.last_progress_ms) >= HISTORY_TRANSFER_PROGRESS_MS)
    {
        s_transfer.pending.final = 2U;
        s_transfer.pending.text[0] = '\0';
        s_transfer.pending_valid = 1U;
    }
}
