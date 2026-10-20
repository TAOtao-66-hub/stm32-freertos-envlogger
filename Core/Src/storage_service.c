#include "storage_service.h"
#include "history_buffer.h"
#include "history_transfer.h"
#include "w25qxx.h"
#include "main.h"
#include "project_config.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

static QueueHandle_t s_request_queue;
static QueueHandle_t s_sample_queue;
static QueueHandle_t s_saved_queue;
static QueueHandle_t s_text_queue;
static uint32_t s_write_count;
static uint32_t s_error_count;
static uint32_t s_failed_record_count;

typedef enum
{
    STORAGE_DRAIN_EMPTY = 0,
    STORAGE_DRAIN_WRITTEN,
    STORAGE_DRAIN_ERROR
} Storage_DrainStatus;

static Storage_DrainStatus write_next_record(void)
{
    History_Info info;
    History_Sample record;

    HistoryStore_GetInfo(&info);
    /* Preserve the remaining FIFO after initialization/uncertain-write failure. */
    if (info.ready == 0U) { return STORAGE_DRAIN_ERROR; }
    if (xQueueReceive(s_sample_queue, &record, 0U) != pdPASS) { return STORAGE_DRAIN_EMPTY; }
    if (HistoryStore_Append(&record) == 0U)
    {
        ++s_failed_record_count;
        ++s_error_count;
        return STORAGE_DRAIN_ERROR; /* Do not replay an uncertain write. */
    }
    ++s_write_count;
    return STORAGE_DRAIN_WRITTEN;
}

static uint8_t flush_before_time_anchor(void)
{
    uint32_t index;
    Storage_DrainStatus status;

    /* All records created before calibration must precede its anchor on
     * media, so the existing reverse scan can infer their dates. New records
     * capture calibrated time. Bound this work by the FIFO's initial capacity. */
    for (index = 0U; index < HISTORY_RECORD_QUEUE_LENGTH; ++index)
    {
        status = write_next_record();
        if (status == STORAGE_DRAIN_EMPTY) { return 1U; }
        if (status == STORAGE_DRAIN_ERROR) { return 0U; }
    }
    return 1U;
}

static uint8_t send_text(Storage_TextReply *reply)
{
    return (xQueueSend(s_text_queue, reply, pdMS_TO_TICKS(STORAGE_REPLY_TIMEOUT_MS)) == pdPASS) ? 1U : 0U;
}

static const char *history_error_name(History_Error error)
{
    switch (error)
    {
        case HISTORY_ERROR_NONE:            return "NONE";
        case HISTORY_ERROR_MEDIA_NOT_READY: return "MEDIA_NOT_READY";
        case HISTORY_ERROR_READ_HEADER:     return "READ_HEADER";
        case HISTORY_ERROR_HEADER_UNKNOWN:  return "HEADER_UNKNOWN";
        case HISTORY_ERROR_READ_SLOT:       return "READ_SLOT";
        case HISTORY_ERROR_ERASE_SECTOR:    return "ERASE_SECTOR";
        case HISTORY_ERROR_WRITE_HEADER:    return "WRITE_HEADER";
        case HISTORY_ERROR_WRITE_RECORD:    return "WRITE_RECORD";
        default:                           return "UNKNOWN";
    }
}

static const char *flash_stage_name(W25Q_IoStage stage)
{
    switch (stage)
    {
        case W25Q_IO_STATUS:     return "STATUS";
        case W25Q_IO_TRANSMIT:   return "TX";
        case W25Q_IO_READ_DATA:  return "READ_DATA";
        case W25Q_IO_IDENTIFY:   return "IDENTIFY";
        case W25Q_IO_WAIT_READY: return "WAIT_READY";
        case W25Q_IO_RECOVERY:   return "RECOVERY";
        default:                return "NONE";
    }
}

void StorageService_Init(QueueHandle_t request, QueueHandle_t sample,
                         QueueHandle_t boot, QueueHandle_t saved, QueueHandle_t text)
{
    Storage_Boot result;
    EnvAlarm_State defaults;
    s_request_queue = request;
    s_sample_queue = sample;
    s_saved_queue = saved;
    s_text_queue = text;
    HistoryTransfer_Init(text);
    s_write_count = 0U;
    s_error_count = 0U;
    s_failed_record_count = 0U;
    EnvAlarm_Init(&defaults);
    result.config = defaults.config;
    result.status = SETTINGS_STORE_ERROR;
    if (W25Q_Init() != 0U)
    {
        result.status = SettingsStore_Load(&result.config);
        if (HistoryStore_Open() == 0U) { ++s_error_count; }
    }
    else { ++s_error_count; }
    /* This mailbox is always published, including missing/unsupported Flash. */
    (void)xQueueOverwrite(boot, &result);
}

static void handle_request(const Storage_Request *request)
{
    Storage_SaveReply saved;
    Storage_TextReply reply;
    W25Q_Info flash;
    W25Q_Diagnostic flash_diagnostic;
    History_Info history;
    History_Diagnostic diagnostic;
    HistoryBuffer_Stats buffer;
    History_Record record;
    History_ReadStatus read_status;
    TimeService_Snapshot anchor;
    const char *time_source;
    char date[20];
    uint16_t index;
    uint32_t discarded;

    if ((request->kind == STORAGE_REQUEST_PAGE) || (request->kind == STORAGE_REQUEST_EXPORT))
    {
        if (HistoryTransfer_Begin(request) == 0U)
        {
            reply.request_id = request->request_id;
            reply.final = 1U;
            (void)strcpy(reply.text, "ERR TRANSFER_BUSY; RETRY\r\n");
            (void)send_text(&reply);
        }
        return;
    }
    if (request->kind == STORAGE_REQUEST_CANCEL)
    {
        HistoryTransfer_Cancel(request->request_id);
        return;
    }
    if (request->kind == STORAGE_REQUEST_SAVE)
    {
        saved.request_id = request->request_id;
        saved.status = SettingsStore_Save(&request->config);
        if (saved.status == SETTINGS_STORE_ERROR) { ++s_error_count; }
        (void)xQueueOverwrite(s_saved_queue, &saved);
        return;
    }
    reply.request_id = request->request_id;
    reply.final = 1U;
    W25Q_GetInfo(&flash);
    W25Q_GetDiagnostic(&flash_diagnostic);
    HistoryStore_GetInfo(&history);
    if (request->kind == STORAGE_REQUEST_CLEAR)
    {
        if (history.ready == 0U)
        { (void)strcpy(reply.text, "ERR LOG_NOT_READY; CHECK flash info AND log status\r\n"); }
        else if (HistoryTransfer_IsActive() != 0U)
        { (void)strcpy(reply.text, "ERR TRANSFER_BUSY; RETRY\r\n"); }
        else
        {
            HistoryBuffer_SetPaused(1U);
            if (HistoryStore_Clear() == 0U)
            {
                ++s_error_count;
                (void)strcpy(reply.text, "ERR LOG_CLEAR_IO; STATE_UNCERTAIN; RESTART_AND_CHECK\r\n");
            }
            else
            {
                discarded = HistoryBuffer_DiscardPending();
                HistoryStore_GetInfo(&history);
                (void)snprintf(reply.text, sizeof(reply.text),
                               "OK log clear; next=%lu discarded=%lu; SETTINGS_KEPT\r\n",
                               (unsigned long)history.next_sequence, (unsigned long)discarded);
            }
            HistoryBuffer_SetPaused(0U);
        }
    }
    else if (request->kind == STORAGE_REQUEST_TIME_SET)
    {
        if (TimeService_SetUnix(request->timestamp) == 0U)
        {
            (void)strcpy(reply.text, "ERR TIME_RTC; CLOCK_NOT_SET\r\n");
        }
        else
        {
            TimeService_Get(&anchor);
            if ((flush_before_time_anchor() != 0U) && (HistoryStore_AppendTimeAnchor(&anchor) != 0U))
            {
                (void)strcpy(reply.text, "OK time set; beijingtime; anchor=STORED\r\n");
            }
            else
            {
                ++s_error_count;
                (void)strcpy(reply.text, "ERR TIME_ANCHOR; CLOCK_SET beijingtime; NOT_SAVED\r\n");
            }
        }
    }
    else if (request->kind == STORAGE_REQUEST_INFO)
    {
        (void)snprintf(reply.text, sizeof(reply.text), "FLASH id=%06lX bytes=%lu ready=%u\r\n",
                       (unsigned long)flash.jedec_id, (unsigned long)flash.capacity_bytes, (unsigned int)flash.ready);
    }
    else if (request->kind == STORAGE_REQUEST_STATUS)
    {
        (void)snprintf(reply.text, sizeof(reply.text), "LOG ready=%u slots=%lu next=%lu written=%lu errors=%lu period=%lums\r\n",
                       (unsigned int)history.ready, (unsigned long)history.slot_capacity,
                       (unsigned long)history.next_sequence, (unsigned long)s_write_count,
                       (unsigned long)s_error_count, (unsigned long)HISTORY_RECORD_INTERVAL_MS);
        reply.final = 0U;
        if (send_text(&reply) == 0U) { ++s_error_count; return; }
        HistoryBuffer_GetStats(&buffer);
        reply.final = 1U;
        (void)snprintf(reply.text, sizeof(reply.text),
                       "LOG_QUEUE used=%u cap=%u peak=%u in=%lu full=%lu missed=%lu failed=%lu\r\n",
                       (unsigned int)buffer.used, (unsigned int)HISTORY_RECORD_QUEUE_LENGTH,
                       (unsigned int)buffer.peak, (unsigned long)buffer.accepted,
                       (unsigned long)buffer.dropped_full, (unsigned long)buffer.missed_periods,
                       (unsigned long)s_failed_record_count);
        if ((buffer.discarded_clear != 0U) || (buffer.paused_periods != 0U) || (buffer.paused != 0U))
        {
            reply.final = 0U;
            if (send_text(&reply) == 0U) { ++s_error_count; return; }
            reply.final = 1U;
            (void)snprintf(reply.text, sizeof(reply.text),
                           "LOG_MAINT paused=%u discarded_clear=%lu paused_periods=%lu\r\n",
                           (unsigned int)buffer.paused, (unsigned long)buffer.discarded_clear,
                           (unsigned long)buffer.paused_periods);
        }
        if (history.ready == 0U)
        {
            /* Keep each reply within the existing queue item and UART limit. */
            reply.final = 0U;
            if (send_text(&reply) == 0U) { ++s_error_count; return; }
            HistoryStore_GetDiagnostic(&diagnostic);
            reply.final = (diagnostic.error == HISTORY_ERROR_HEADER_UNKNOWN) ? 0U : 1U;
            (void)snprintf(reply.text, sizeof(reply.text), "LOG_DIAG reason=%s addr=%08lX invalid=%lu\r\n",
                           (flash.ready == 0U) ? "FLASH_NOT_READY" : history_error_name(diagnostic.error),
                           (unsigned long)diagnostic.address, (unsigned long)diagnostic.invalid_headers);
            if (reply.final == 0U)
            {
                if (send_text(&reply) == 0U) { ++s_error_count; return; }
                reply.final = 1U;
                (void)snprintf(reply.text, sizeof(reply.text),
                               "LOG_HEAD magic=%08lX crc=%08lX calc=%08lX commit=%08lX\r\n",
                               (unsigned long)diagnostic.header_magic, (unsigned long)diagnostic.stored_crc,
                               (unsigned long)diagnostic.computed_crc, (unsigned long)diagnostic.commit);
            }
        }
    }
    else if (request->kind == STORAGE_REQUEST_READ)
    {
        if ((flash.ready == 0U) || (history.ready == 0U))
        {
            (void)strcpy(reply.text, "ERR LOG_NOT_READY; CHECK flash info AND log status\r\n");
            (void)send_text(&reply);
            return;
        }
        reply.final = 0U;
        for (index = 0U; index < request->count; ++index)
        {
            read_status = HistoryStore_ReadRecent(index, &record);
            if (read_status == HISTORY_READ_EMPTY) { break; }
            if (read_status == HISTORY_READ_ERROR)
            {
                ++s_error_count;
                reply.final = 1U;
                (void)strcpy(reply.text, "ERR LOG_READ_IO\r\n");
                (void)send_text(&reply);
                return;
            }
            if (record.time_source != HISTORY_TIME_UNKNOWN) { TimeService_Format(record.display_timestamp, date); }
            else { (void)strcpy(date, "UNCALIBRATED"); }
            time_source = (record.time_source == HISTORY_TIME_RTC) ? "rtc" :
                          ((record.time_source == HISTORY_TIME_ESTIMATED) ? "estimated" : "unknown");
            (void)snprintf(reply.text, sizeof(reply.text), "LOG %lu,%lu,%lu,%s,%u.%u,%u.%u,valid=%u,alarm=%u,time=%s\r\n",
                           (unsigned long)record.sequence, (unsigned long)record.session,
                           (unsigned long)record.value.time.uptime_seconds, date,
                           (unsigned int)(record.value.sample.temperature_x10 / 10U),
                           (unsigned int)(record.value.sample.temperature_x10 % 10U),
                           (unsigned int)(record.value.sample.humidity_x10 / 10U),
                           (unsigned int)(record.value.sample.humidity_x10 % 10U),
                           (unsigned int)record.value.sample.valid, (unsigned int)record.value.alarm_flags, time_source);
            if (send_text(&reply) == 0U) { ++s_error_count; return; }
        }
        reply.final = 1U;
        (void)snprintf(reply.text, sizeof(reply.text), "OK log read count=%u; NEWEST_FIRST\r\n", (unsigned int)index);
    }
    else { (void)strcpy(reply.text, "ERR STORAGE_REQUEST\r\n"); }
    if (((request->kind == STORAGE_REQUEST_INFO) ||
         ((request->kind == STORAGE_REQUEST_STATUS) && (history.ready == 0U))) &&
        (flash_diagnostic.stage != W25Q_IO_NONE))
    {
        reply.final = 0U;
        if (send_text(&reply) == 0U) { ++s_error_count; return; }
        reply.final = 1U;
        (void)snprintf(reply.text, sizeof(reply.text),
                       "FLASH_IO stage=%s hal=%lu err=%08lX ms=%lu retries=%lu addr=%08lX\r\n",
                       flash_stage_name(flash_diagnostic.stage), (unsigned long)flash_diagnostic.hal_status,
                       (unsigned long)flash_diagnostic.hal_error, (unsigned long)flash_diagnostic.elapsed_ms,
                       (unsigned long)flash_diagnostic.read_retries, (unsigned long)flash_diagnostic.address);
    }
    if (send_text(&reply) == 0U) { ++s_error_count; }
}

void StorageService_Poll(void)
{
    Storage_Request request;
    if (xQueueReceive(s_request_queue, &request, 0U) == pdPASS) { handle_request(&request); }
    HistoryTransfer_Poll();
    /* One queued record per poll keeps command handling responsive. */
    (void)write_next_record();
}
