#include "project_commands.h"
#include "potentiometer.h"
#include "serial_port.h"
#include "storage_codec.h"
#include "storage_service.h"
#include "history_buffer.h"
#include "w25qxx.h"
#include "serial_command.h"
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
static uint32_t text_capacity = 4U;
static uint32_t read_calls;
static uint32_t text_blocking;
static uint8_t tx_failure;
static uint32_t control_sends;

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
    ++read_calls;
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
    if (queue == (void *)6) { ++control_sends; return pdPASS; }
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
    if ((queue == TEXT_QUEUE) && (delay != 0U)) { ++text_blocking; }
    if ((queue == TEXT_QUEUE) && (reply_write - reply_read < text_capacity))
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
    if (tx_failure != 0U) { tx_failure = 0U; return HAL_ERROR; }
    if (strlen(output) + strlen(text) >= sizeof(output)) { return HAL_ERROR; }
    (void)strcat(output, text);
    return HAL_OK;
}
void Pot_Get(Pot_Snapshot *snapshot) { (void)memset(snapshot, 0, sizeof(*snapshot)); }
HAL_StatusTypeDef SerialPort_Write(const uint8_t *data, uint16_t length)
{
    char text[129];
    if (length >= sizeof(text)) { return HAL_ERROR; }
    (void)memcpy(text, data, length);
    text[length] = '\0';
    return SerialPort_WriteString(text);
}
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
    reply_read = reply_write = 0U;
    text_capacity = 4U;
    read_calls = text_blocking = 0U;
    tx_failure = 0U;
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


/* End-to-end tests use the same NOR/queue boundary model as the legacy
 * service suite; production history/transfer/storage/commands are linked. */
#include "history_transfer.h"

static void seed(uint32_t count)
{
    History_Sample sample = {{255U, 580U, 1U}, {0U, 0U, 0U}, 0U};
    uint32_t index;
    for (index = 0U; index < count; ++index)
    {
        sample.time.uptime_seconds = (index + 1U) * 10U;
        (void)HistoryStore_Append(&sample);
    }
}

static void start(const char *command)
{
    uint8_t handled;
    (void)ProjectCommands_Execute(command, &handled);
}

static void feed(const char *text)
{
    while (*text != '\0') { (void)SerialCommand_ProcessByte((uint8_t)*text++, ticks); }
}

static uint8_t pump(uint32_t limit)
{
    while (limit-- > 0U)
    {
        StorageService_Poll();
        ProjectCommands_Service();
        ticks += 10U;
        if ((ProjectCommands_TransferActive() == 0U) &&
            (request_ready == 0U) && (reply_read == reply_write)) { return 1U; }
    }
    return 0U;
}

uint32_t RunHistoryTransferTests(void)
{
    uint32_t index;
    uint32_t before;
    uint32_t request_id;
    uint8_t handled;
    History_Reader reader;
    History_Record record;
    History_ReadStatus status;
    Storage_Request request = {0};
    failures = 0U;

    reset_fifo_scenario();
    start("log export");
    CHECK(0, pump(30U) && strstr(output, "snapshot=0") != NULL &&
             strstr(output, "sequence,session,uptime_s,beijingtime") != NULL &&
             strstr(output, "CSV_END rows=0 skipped=0 state=OK") != NULL);

    reset_fifo_scenario(); seed(40U);
    start("log export");
    CHECK(1, pump(200U) && strstr(output, "CSV_END rows=40 skipped=0 state=OK") != NULL &&
             strstr(output, "40,1,400,UNCALIBRATED,25.5,58.0,1,0,unknown") != NULL);
    CHECK(2, text_blocking == 0U && strstr(output, "LOG ") == NULL && strlen(output) < sizeof(output));

    reset_fifo_scenario(); seed(25U); start("log page 0 10");
    CHECK(3, pump(100U) && strstr(output, "PAGE_END count=10 next=16 skipped=0 state=OK") != NULL &&
             strstr(output, "LOG 25,") != NULL && strstr(output, "LOG 15,") == NULL);
    seed(1U); output[0] = '\0'; start("log page 16 10");
    CHECK(4, pump(100U) && strstr(output, "LOG 15,") != NULL && strstr(output, "LOG 16,") == NULL &&
             strstr(output, "PAGE_END count=10 next=6") != NULL);
    output[0] = '\0'; start("log page 6 10");
    CHECK(5, pump(100U) && strstr(output, "LOG 5,") != NULL && strstr(output, "LOG 1,") != NULL &&
             strstr(output, "PAGE_END count=5 next=0") != NULL);
    output[0] = '\0'; start("log page 1 10");
    CHECK(5, pump(100U) && strstr(output, "PAGE_END count=0 next=0") != NULL);

    reset_fifo_scenario();
    start("log page 4294967296 10"); start("log page -1 10");
    start("log page 0 0"); start("log page 0 21"); start("log page 0 1x");
    CHECK(6, request_ready == 0U && strstr(output, "ERR LOG_PAGE") != NULL);
    output[0] = '\0'; start("log page 4294967295 20");
    CHECK(6, request_ready != 0U && request_mailbox.before_sequence == 0xFFFFFFFFUL && pump(30U));

    reset_fifo_scenario(); seed(30U); start("log export");
    StorageService_Poll(); /* Capture snapshot before a queued sample is appended. */
    produce_record(1000U, 260U);
    for (index = 0U; index < 20U; ++index) { StorageService_Poll(); ticks += 10U; }
    CHECK(7, fifo_read == fifo_write && reply_write - reply_read == 4U && text_blocking == 0U);
    CHECK(7, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK && record.sequence == 31U);
    CHECK(8, pump(200U) && strstr(output, "CSV_END rows=30 skipped=0 state=OK") != NULL &&
             strstr(output, "31,1,") == NULL);

    reset_fifo_scenario(); seed(40U); start("log export");
    for (index = 0U; index < 12U; ++index) { StorageService_Poll(); ProjectCommands_Service(); }
    start("help"); start("time get"); start("set temp 30");
    CHECK(9, strstr(output, "commands:") == NULL && strstr(output, "TIME ") == NULL &&
             strstr(output, "ERR ") == NULL && request_ready == 0U);
    ProjectCommands_DeferNotice(); start("log cancel"); ProjectCommands_Service();
    CHECK(10, pump(40U) && strstr(output, "state=CANCELLED") != NULL &&
              strstr(output, "ERR TRANSFER_BUSY") > strstr(output, "CSV_END"));
    output[0] = '\0'; start("log cancel");
    CHECK(10, strstr(output, "ERR NO_ACTIVE_TRANSFER") != NULL);

    reset_fifo_scenario(); seed(20U); start("log export");
    for (index = 0U; index < 20U; ++index) { StorageService_Poll(); }
    start("log cancel"); ProjectCommands_Service();
    CHECK(11, pump(50U) && strstr(output, "state=CANCELLED") != NULL && text_blocking == 0U);

    reset_fifo_scenario(); seed(5U);
    media[2U * 4096U + 3U * 32U + 20U] ^= 1U;
    start("log export");
    CHECK(12, pump(60U) && strstr(output, "rows=4 skipped=1 state=OK") != NULL &&
              strstr(output, "3,1,30,") == NULL);

    reset_fifo_scenario(); seed(5U); start("log export");
    StorageService_Poll(); read_failure_address = 2U * 4096U;
    CHECK(13, pump(40U) && strstr(output, "state=READ_IO") != NULL);

    reset_fifo_scenario(); read_failure_address = 2U * 4096U;
    (void)HistoryStore_Open(); start("log export");
    CHECK(14, pump(30U) && strstr(output, "state=NOT_READY") != NULL);

    reset_fifo_scenario(); seed(254U);
    CHECK(15, HistoryStore_BeginRead(&reader, 0U) != 0U);
    seed(1U); status = HISTORY_READ_PENDING;
    for (index = 0U; index < 300U; ++index)
    {
        before = read_calls;
        status = HistoryStore_ReadNext(&reader, &record, 8U);
        CHECK(16, read_calls - before <= 8U);
        if (status == HISTORY_READ_OVERRUN || status == HISTORY_READ_ERROR || status == HISTORY_READ_EMPTY) { break; }
        if (status == HISTORY_READ_OK) { CHECK(15, record.sequence <= 254U); }
    }
    CHECK(15, status == HISTORY_READ_OVERRUN);

    reset_fifo_scenario(); seed(5U);
    clock_value.uptime_seconds = 80U; start("time set 2026 10 19 15:30:00");
    StorageService_Poll(); drain_replies(); output[0] = '\0'; start("log export");
    CHECK(17, pump(60U) && strstr(output, "CSV_END rows=5 skipped=0 state=OK") != NULL &&
              strstr(output, "2026 10 19 15:29:30,25.5,58.0,1,0,estimated") != NULL);

    reset_fifo_scenario(); start("log export"); request_id = request_mailbox.request_id;
    request_ready = 0U; /* Feed only internal heartbeats: no printed keepalive. */
    for (index = 0U; index < 6U; ++index)
    {
        ticks += 9000U;
        replies[reply_write % 24U].request_id = request_id;
        replies[reply_write++ % 24U].final = 2U;
        ProjectCommands_Service();
    }
    CHECK(18, ProjectCommands_TransferActive() != 0U && output[0] == '\0');
    ticks += 10001U; ProjectCommands_Service();
    CHECK(19, ProjectCommands_TransferActive() == 0U && strstr(output, "ERR STORAGE_TIMEOUT") != NULL);
    ProjectCommands_Service();
    CHECK(19, request_ready != 0U && request_mailbox.kind == STORAGE_REQUEST_CANCEL &&
              request_mailbox.request_id == request_id);

    reset_fifo_scenario(); seed(3U); start("log export"); StorageService_Poll(); StorageService_Poll();
    tx_failure = 1U; ProjectCommands_Service(); ProjectCommands_Service();
    CHECK(20, ProjectCommands_TransferActive() == 0U && strstr(output, "ERR STORAGE_TX") != NULL &&
              request_mailbox.kind == STORAGE_REQUEST_CANCEL);

    reset_fifo_scenario(); seed(3U); start("log export"); StorageService_Poll();
    request.kind = STORAGE_REQUEST_EXPORT; request.request_id = 99U;
    CHECK(21, HistoryTransfer_Begin(&request) == 0U);
    CHECK(21, pump(40U) && strstr(output, "CSV_END rows=3") != NULL);

    reset_fifo_scenario(); seed(3U); start("log page 0 3");
    CHECK(22, pump(40U) && strstr(output, "PAGE_END count=3 next=0") != NULL);

    reset_fifo_scenario(); seed(2U); start("log export"); StorageService_Poll();
    request_id = request_mailbox.request_id;
    /* Wrong-ID replies must not close or expose text inside the stream. */
    replies[reply_write % 24U].request_id = request_id + 1U;
    replies[reply_write % 24U].final = 1U;
    (void)strcpy(replies[reply_write++ % 24U].text, "FOREIGN\r\n");
    ProjectCommands_Service();
    CHECK(23, ProjectCommands_TransferActive() != 0U && strstr(output, "FOREIGN") == NULL && pump(40U));

    reset_fifo_scenario(); seed(3U); SerialCommand_Init();
    SerialCommand_BindAlarmQueue((void *)6); control_sends = 0U;
    feed("set temp 30\r"); feed("log export\r");
    CHECK(24, control_sends == 1U && request_ready == 0U &&
              ProjectCommands_TransferActive() == 0U && strstr(output, "ERR BUSY") != NULL);

    reset_fifo_scenario(); seed(3U); SerialCommand_Init();
    SerialCommand_BindAlarmQueue((void *)6); control_sends = 0U;
    feed("LoG ExPoRt\r"); StorageService_Poll();
    feed("set temp 30\r"); feed("get\r");
    CHECK(25, control_sends == 0U && output[0] == '\0' && ProjectCommands_TransferActive() != 0U);
    for (index = 0U; index < SERIAL_COMMAND_MAX_LENGTH + 2U; ++index)
    { (void)SerialCommand_ProcessByte((uint8_t)'x', ticks); }
    feed("\r");
    CHECK(26, output[0] == '\0');
    SerialCommand_DiscardLine(ticks); feed("\r");
    CHECK(26, output[0] == '\0');
    feed("log cancel\r"); ProjectCommands_Service();
    CHECK(27, pump(40U) && strstr(output, "state=CANCELLED") != NULL &&
              strstr(output, "ERR TRANSFER_BUSY") > strstr(output, "CSV_END"));
    (void)handled;
    return failures;
}
