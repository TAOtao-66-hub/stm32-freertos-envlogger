#include "project_commands.h"
#include "potentiometer.h"
#include "serial_port.h"
#include "storage_codec.h"
#include "storage_service.h"
#include "history_buffer.h"
#include "w25qxx.h"
#include <string.h>
/* UART command routing only; actual kernel diagnostics have a separate suite. */
static uint32_t diagnostic_calls;
HAL_StatusTypeDef RtosDiagnostics_Print(void) { ++diagnostic_calls; return HAL_OK; }

#define REQUEST_QUEUE ((void *)1)
#define TEXT_QUEUE    ((void *)2)
#define SAMPLE_QUEUE  ((void *)3)
#define BOOT_QUEUE    ((void *)4)
#define SAVED_QUEUE   ((void *)5)

static uint8_t media[4U * 4096U];
static uint32_t ticks;
static int32_t program_budget;
static uint8_t rtc_failure;
static uint32_t read_failure_address = 0xFFFFFFFFUL;
static W25Q_Diagnostic flash_diagnostic;
static uint8_t request_ready;
static uint8_t boot_published;
static Storage_Request request_mailbox;
static Storage_TextReply replies[24];
static uint32_t reply_read;
static uint32_t reply_write;
static TimeService_Snapshot clock_value;
static char output[4096];
static uint32_t failures;
static History_Sample fifo[HISTORY_RECORD_QUEUE_LENGTH];
static uint32_t fifo_read;
static uint32_t fifo_write;
static uint32_t blocking_sends;
static uint32_t sample_overwrites;

#define CHECK(group, condition) do { if (!(condition)) { failures |= 1UL << (group); } } while (0)

uint32_t HAL_GetTick(void) { return ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
uint8_t W25Q_Init(void) { return 1U; }
void W25Q_GetDiagnostic(W25Q_Diagnostic *diagnostic) { *diagnostic = flash_diagnostic; }
void W25Q_GetInfo(W25Q_Info *info)
{
    info->ready = 1U;
    info->capacity_bytes = sizeof(media);
    info->jedec_id = 0xEF4016UL;
}
uint8_t W25Q_Read(uint32_t address, void *data, uint32_t length)
{
    if ((address == read_failure_address) || (address >= sizeof(media)) ||
        (length > sizeof(media) - address)) { return 0U; }
    (void)memcpy(data, media + address, length);
    return 1U;
}
uint8_t W25Q_Program(uint32_t address, const void *data, uint32_t length)
{
    const uint8_t *source = (const uint8_t *)data;
    if ((address >= sizeof(media)) || (length > sizeof(media) - address)) { return 0U; }
    while (length-- > 0U)
    {
        if (program_budget == 0) { return 0U; }
        if (program_budget > 0) { --program_budget; }
        if ((media[address] & *source) != *source) { return 0U; }
        media[address++] &= *source++;
    }
    return 1U;
}
uint8_t W25Q_EraseSector(uint32_t address)
{
    if ((address >= sizeof(media)) || ((address % 4096U) != 0U)) { return 0U; }
    (void)memset(media + address, 0xFF, 4096U);
    return 1U;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t delay)
{
    if (queue == SAMPLE_QUEUE)
    {
        if (delay != 0U) { ++blocking_sends; }
        if (fifo_write - fifo_read >= HISTORY_RECORD_QUEUE_LENGTH) { return pdFAIL; }
        fifo[fifo_write++ % HISTORY_RECORD_QUEUE_LENGTH] = *(const History_Sample *)item;
        return pdPASS;
    }
    if (queue == REQUEST_QUEUE)
    {
        if (request_ready != 0U) { return pdFAIL; }
        request_mailbox = *(const Storage_Request *)item;
        request_ready = 1U;
        return pdPASS;
    }
    if ((queue == TEXT_QUEUE) && (reply_write - reply_read < 24U))
    {
        replies[reply_write++ % 24U] = *(const Storage_TextReply *)item;
        return pdPASS;
    }
    return pdFAIL;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t delay)
{
    (void)delay;
    if ((queue == SAMPLE_QUEUE) && (fifo_read < fifo_write))
    {
        *(History_Sample *)item = fifo[fifo_read++ % HISTORY_RECORD_QUEUE_LENGTH];
        return pdPASS;
    }
    if ((queue == REQUEST_QUEUE) && (request_ready != 0U))
    {
        *(Storage_Request *)item = request_mailbox;
        request_ready = 0U;
        return pdPASS;
    }
    if ((queue == TEXT_QUEUE) && (reply_read < reply_write))
    {
        *(Storage_TextReply *)item = replies[reply_read++ % 24U];
        return pdPASS;
    }
    return pdFAIL;
}
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    (void)item;
    if (queue == SAMPLE_QUEUE) { ++sample_overwrites; return pdFAIL; }
    if (queue == BOOT_QUEUE) { boot_published = 1U; return pdPASS; }
    return (queue == SAVED_QUEUE) ? pdPASS : pdFAIL;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    return (queue == SAMPLE_QUEUE) ? fifo_write - fifo_read : 0U;
}
HAL_StatusTypeDef SerialPort_WriteString(const char *text)
{
    if (strlen(output) + strlen(text) >= sizeof(output)) { return HAL_ERROR; }
    (void)strcat(output, text);
    return HAL_OK;
}
void Pot_Get(Pot_Snapshot *snapshot) { (void)memset(snapshot, 0, sizeof(*snapshot)); }
void TimeService_Get(TimeService_Snapshot *snapshot) { *snapshot = clock_value; }
uint8_t TimeService_SetUnix(uint32_t timestamp)
{
    if (rtc_failure != 0U) { return 0U; }
    clock_value.timestamp = timestamp;
    clock_value.calibrated = 1U;
    return 1U;
}

static void drain_replies(void)
{
    while (reply_read < reply_write) { ProjectCommands_Service(); }
}

static void reset_fifo_scenario(void)
{
    (void)memset(media, 0xFF, sizeof(media));
    (void)memset(&flash_diagnostic, 0, sizeof(flash_diagnostic));
    (void)memset(&clock_value, 0, sizeof(clock_value));
    program_budget = -1;
    read_failure_address = 0xFFFFFFFFUL;
    rtc_failure = request_ready = 0U;
    ticks = fifo_read = fifo_write = 0U;
    output[0] = '\0';
    StorageService_Init(REQUEST_QUEUE, SAMPLE_QUEUE, BOOT_QUEUE, SAVED_QUEUE, TEXT_QUEUE);
    HistoryBuffer_Init(SAMPLE_QUEUE, 0U);
    ProjectCommands_Bind(REQUEST_QUEUE, TEXT_QUEUE);
}

static void produce_record(uint32_t seconds, uint16_t temperature)
{
    EnvAlarm_Sample sample = {0U, 600U, 1U};
    sample.temperature_x10 = temperature;
    ticks = seconds * 1000U;
    clock_value.uptime_seconds = seconds;
    HistoryBuffer_Poll(ticks, &sample, 0U);
}

uint32_t RunTimeHistoryServiceTests(void)
{
    uint8_t handled;
    History_Sample sample = {{250U, 600U, 1U}, {0U, 20U, 0U}, 0U};
    History_Record record;
    uint32_t next_before;
    History_Info info;
    HistoryBuffer_Stats stats;
    uint32_t index;

    failures = 0U;
    (void)memset(media, 0xFF, sizeof(media));
    program_budget = -1;
    StorageService_Init(REQUEST_QUEUE, SAMPLE_QUEUE, BOOT_QUEUE, SAVED_QUEUE, TEXT_QUEUE);
    HistoryBuffer_Init(SAMPLE_QUEUE, ticks);
    ProjectCommands_Bind(REQUEST_QUEUE, TEXT_QUEUE);
    CHECK(0, boot_published != 0U && HistoryStore_Append(&sample) != 0U);
    clock_value.uptime_seconds = 80U;
    (void)ProjectCommands_Execute("time set 2026 10 19 15:30:00", &handled);
    CHECK(0, handled != 0U && request_ready != 0U && output[0] == '\0' && clock_value.calibrated == 0U);
    StorageService_Poll();
    CHECK(0, output[0] == '\0' && clock_value.calibrated != 0U);
    CHECK(0, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
             record.time_source == HISTORY_TIME_ESTIMATED && record.display_timestamp == 1792394940UL);
    drain_replies();
    CHECK(0, strcmp(output, "OK time set; beijingtime; anchor=STORED\r\n") == 0);

    output[0] = '\0';
    (void)ProjectCommands_Execute("log read 1", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(1, strstr(output, "2026 10 19 15:29:00") != NULL &&
             strstr(output, "time=estimated") != NULL && strstr(output, "count=1") != NULL);

    /* Restart changes current session/RTC validity, not old persisted dates. */
    CHECK(2, HistoryStore_Open() != 0U);
    clock_value.calibrated = 0U;
    clock_value.uptime_seconds = 5U;
    output[0] = '\0';
    (void)ProjectCommands_Execute("time get", &handled);
    CHECK(2, strstr(output, "UNCALIBRATED") != NULL);
    output[0] = '\0';
    (void)ProjectCommands_Execute("log read 1", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(2, strstr(output, "2026 10 19 15:29:00") != NULL && strstr(output, "time=estimated") != NULL);

    sample.time.uptime_seconds = 5U;
    (void)HistoryStore_Append(&sample);
    output[0] = '\0';
    (void)ProjectCommands_Execute("log read 2", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(3, strstr(output, "UNCALIBRATED") != NULL && strstr(output, "time=unknown") != NULL &&
             strstr(output, "2026 10 19 15:29:00") != NULL);

    /* A failed RTC operation cannot publish a successful anchor response. */
    output[0] = '\0';
    rtc_failure = 1U;
    HistoryStore_GetInfo(&info);
    next_before = info.next_sequence;
    (void)ProjectCommands_Execute("time set 2026 10 19 16:00:00", &handled);
    StorageService_Poll();
    drain_replies();
    HistoryStore_GetInfo(&info);
    CHECK(4, strstr(output, "ERR TIME_RTC") != NULL && strstr(output, "OK time set") == NULL &&
             info.next_sequence == next_before && clock_value.calibrated == 0U);
    rtc_failure = 0U;

    output[0] = '\0';
    (void)ProjectCommands_Execute("time set 2026 02 29 12:00:00", &handled);
    CHECK(5, strstr(output, "USE time set YYYY MM DD HH:MM:SS") != NULL && request_ready == 0U);

    /* Cut an anchor write: report clock-set but not persisted, never OK. */
    output[0] = '\0';
    program_budget = 10;
    (void)ProjectCommands_Execute("time set 2026 10 19 16:00:00", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(6, clock_value.calibrated != 0U && strstr(output, "ERR TIME_ANCHOR; CLOCK_SET") != NULL &&
             strstr(output, "NOT_SAVED") != NULL && strstr(output, "OK time set") == NULL);
    program_budget = -1;
    CHECK(6, HistoryStore_Open() != 0U);
    CHECK(6, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK && record.time_source == HISTORY_TIME_UNKNOWN);

    /* A future calibrated sample remains explicitly RTC-stamped. */
    output[0] = '\0';
    (void)ProjectCommands_Execute("time set 2026 10 19 17:00:00", &handled);
    StorageService_Poll();
    drain_replies();
    sample.time = clock_value;
    sample.time.timestamp += 10U;
    sample.time.uptime_seconds += 10U;
    CHECK(7, HistoryStore_Append(&sample) != 0U);
    output[0] = '\0';
    (void)ProjectCommands_Execute("log read 1", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(7, strstr(output, "2026 10 19 17:00:10") != NULL && strstr(output, "time=rtc") != NULL);

    /* Healthy status now has a separate FIFO statistics reply. */
    output[0] = '\0';
    next_before = reply_write;
    (void)ProjectCommands_Execute("log status", &handled);
    StorageService_Poll();
    CHECK(8, reply_write == next_before + 2U && replies[next_before % 24U].final == 0U &&
             replies[(next_before + 1U) % 24U].final == 1U);
    drain_replies();
    CHECK(8, strstr(output, "LOG ready=1") != NULL && strstr(output, "LOG_DIAG") == NULL &&
             strstr(output, "LOG_QUEUE used=0 cap=8 peak=0 in=0 full=0 missed=0 failed=0\r\n") != NULL);

    (void)memset(media, 0xFF, sizeof(media));
    media[8192U] = 0x12U;
    CHECK(9, HistoryStore_Open() == 0U);
    output[0] = '\0';
    next_before = reply_write;
    (void)ProjectCommands_Execute("log status", &handled);
    StorageService_Poll();
    CHECK(9, reply_write == next_before + 4U && replies[(next_before + 2U) % 24U].final == 0U &&
             replies[(next_before + 3U) % 24U].final == 1U);
    drain_replies();
    CHECK(9, strstr(output, "LOG ready=0 slots=0 next=1") != NULL &&
             strstr(output, "reason=HEADER_UNKNOWN addr=00002000 invalid=1\r\n") != NULL &&
             strstr(output, "LOG_HEAD magic=FFFFFF12 crc=FFFFFFFF calc=") != NULL &&
             strstr(output, "commit=FFFFFFFF\r\n") != NULL && media[8192U] == 0x12U);
    output[0] = '\0';
    (void)ProjectCommands_Execute("log read 3", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(9, strstr(output, "ERR LOG_NOT_READY") != NULL);

    (void)memset(media, 0xFF, sizeof(media));
    read_failure_address = 12288U;
    CHECK(10, HistoryStore_Open() == 0U);
    output[0] = '\0';
    next_before = reply_write;
    (void)ProjectCommands_Execute("log status", &handled);
    StorageService_Poll();
    CHECK(10, reply_write == next_before + 3U && replies[(next_before + 1U) % 24U].final == 0U &&
              replies[(next_before + 2U) % 24U].final == 1U);
    drain_replies();
    CHECK(10, strstr(output, "reason=READ_HEADER addr=00003000 invalid=0\r\n") != NULL &&
              strstr(output, "LOG_HEAD") == NULL);

    flash_diagnostic.stage = W25Q_IO_READ_DATA;
    flash_diagnostic.address = 0x000DE000UL;
    flash_diagnostic.hal_status = 3U;
    flash_diagnostic.elapsed_ms = 251U;
    flash_diagnostic.read_retries = 2U;
    output[0] = '\0';
    next_before = reply_write;
    (void)ProjectCommands_Execute("log status", &handled);
    StorageService_Poll();
    CHECK(11, reply_write == next_before + 4U && replies[(next_before + 2U) % 24U].final == 0U &&
              replies[(next_before + 3U) % 24U].final == 1U);
    drain_replies();
    CHECK(11, strstr(output, "FLASH_IO stage=READ_DATA hal=3 err=00000000 ms=251 retries=2 addr=000DE000\r\n") != NULL);

    output[0] = '\0';
    (void)ProjectCommands_Execute("flash info", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(12, strstr(output, "FLASH id=EF4016") != NULL && strstr(output, "FLASH_IO stage=READ_DATA") != NULL);

    flash_diagnostic.stage = W25Q_IO_WAIT_READY;
    flash_diagnostic.elapsed_ms = 0xFFFFFFFFUL;
    flash_diagnostic.read_retries = 0xFFFFFFFFUL;
    flash_diagnostic.hal_error = 0xFFFFFFFFUL;
    flash_diagnostic.address = 0xFFFFFFFFUL;
    output[0] = '\0';
    (void)ProjectCommands_Execute("flash info", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(13, strstr(output, "ms=4294967295 retries=4294967295 addr=FFFFFFFF\r\n") != NULL);

    /* Pause storage while the independent producer keeps generating records. */
    reset_fifo_scenario();
    produce_record(10U, 251U);
    produce_record(20U, 252U);
    produce_record(30U, 253U);
    HistoryBuffer_GetStats(&stats);
    CHECK(14, stats.used == 3U && stats.accepted == 3U && stats.peak == 3U);
    clock_value.uptime_seconds = 90U;
    for (index = 0U; index < 3U; ++index) { StorageService_Poll(); }
    CHECK(14, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.value.sample.temperature_x10 == 253U && record.value.time.uptime_seconds == 30U);
    CHECK(14, HistoryStore_ReadRecent(2U, &record) == HISTORY_READ_OK &&
              record.value.sample.temperature_x10 == 251U && record.value.time.uptime_seconds == 10U);

    reset_fifo_scenario();
    for (index = 1U; index <= 10U; ++index) { produce_record(index * 10U, (uint16_t)(250U + index)); }
    HistoryBuffer_GetStats(&stats);
    CHECK(15, stats.used == 8U && stats.peak == 8U && stats.accepted == 8U && stats.dropped_full == 2U &&
              blocking_sends == 0U && sample_overwrites == 0U);
    StorageService_Poll();
    CHECK(15, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK && record.value.sample.temperature_x10 == 251U);
    for (index = 1U; index < 8U; ++index) { StorageService_Poll(); }
    CHECK(15, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK && record.value.sample.temperature_x10 == 258U &&
              HistoryStore_ReadRecent(8U, &record) == HISTORY_READ_EMPTY);

    output[0] = '\0';
    (void)ProjectCommands_Execute("log status", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(16, strstr(output, "written=8 errors=0") != NULL &&
              strstr(output, "LOG_QUEUE used=0 cap=8 peak=8 in=8 full=2 missed=0 failed=0\r\n") != NULL);

    reset_fifo_scenario();
    clock_value.timestamp = 1792395000UL;
    clock_value.calibrated = 1U;
    produce_record(10U, 265U);
    clock_value.timestamp += 1000U;
    clock_value.uptime_seconds = 1010U;
    StorageService_Poll();
    CHECK(17, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_RTC && record.value.time.timestamp == 1792395000UL &&
              record.value.time.uptime_seconds == 10U);

    reset_fifo_scenario();
    produce_record(10U, 251U);
    produce_record(20U, 252U);
    produce_record(30U, 253U);
    clock_value.uptime_seconds = 40U;
    (void)ProjectCommands_Execute("time set 2026 10 19 15:30:00", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(18, strstr(output, "anchor=STORED") != NULL && fifo_read == fifo_write);
    for (index = 0U; index < 3U; ++index)
    {
        CHECK(18, HistoryStore_ReadRecent(index, &record) == HISTORY_READ_OK &&
                  record.time_source == HISTORY_TIME_ESTIMATED &&
                  record.display_timestamp == 1792395000UL - (index + 1U) * 10U);
    }

    reset_fifo_scenario();
    produce_record(10U, 251U);
    produce_record(20U, 252U);
    produce_record(30U, 253U);
    program_budget = 10;
    StorageService_Poll();
    StorageService_Poll();
    CHECK(19, fifo_write - fifo_read == 2U);
    (void)ProjectCommands_Execute("log status", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(19, strstr(output, "LOG ready=0") != NULL && strstr(output, "written=0 errors=1") != NULL &&
              strstr(output, "LOG_QUEUE used=2 cap=8 peak=3 in=3 full=0 missed=0 failed=1\r\n") != NULL);

    reset_fifo_scenario();
    read_failure_address = 12288U;
    CHECK(20, HistoryStore_Open() == 0U);
    produce_record(10U, 250U);
    StorageService_Poll();
    CHECK(20, fifo_write - fifo_read == 1U);

    reset_fifo_scenario();
    produce_record(35U, 250U);
    HistoryBuffer_GetStats(&stats);
    CHECK(21, stats.accepted == 1U && stats.missed_periods == 2U);
    produce_record(39U, 251U);
    produce_record(40U, 252U);
    HistoryBuffer_GetStats(&stats);
    CHECK(21, stats.accepted == 2U && stats.missed_periods == 2U && fifo[0].time.uptime_seconds == 35U);

    reset_fifo_scenario();
    HistoryBuffer_Init(SAMPLE_QUEUE, 0xFFFFFF00UL);
    HistoryBuffer_Poll(0xFFFFFF00UL + HISTORY_RECORD_INTERVAL_MS - 1U, &sample.sample, 0U);
    CHECK(22, fifo_write == 0U);
    HistoryBuffer_Poll(0xFFFFFF00UL + HISTORY_RECORD_INTERVAL_MS, &sample.sample, 0U);
    CHECK(22, fifo_write == 1U);

    reset_fifo_scenario();
    HistoryBuffer_Poll(10000U, NULL, 0U);
    sample.sample.valid = 0U;
    sample.sample.temperature_x10 = 400U;
    HistoryBuffer_Poll(20000U, &sample.sample, ENV_ALARM_FLAG_TEMPERATURE);
    CHECK(23, fifo_write == 2U && fifo[0].sample.valid == 0U && fifo[0].alarm_flags == ENV_ALARM_FLAG_SENSOR &&
              fifo[1].sample.temperature_x10 == 0U &&
              fifo[1].alarm_flags == (ENV_ALARM_FLAG_SENSOR | ENV_ALARM_FLAG_TEMPERATURE));
    CHECK(23, blocking_sends == 0U && sample_overwrites == 0U);

    reset_fifo_scenario();
    produce_record(10U, 251U);
    produce_record(20U, 252U);
    produce_record(30U, 253U);
    program_budget = 10;
    clock_value.uptime_seconds = 40U;
    (void)ProjectCommands_Execute("time set 2026 10 19 15:30:00", &handled);
    StorageService_Poll();
    drain_replies();
    CHECK(24, clock_value.calibrated != 0U && strstr(output, "ERR TIME_ANCHOR; CLOCK_SET") != NULL &&
              strstr(output, "anchor=STORED") == NULL && fifo_write - fifo_read == 2U);
    return failures;
}
