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
static uint8_t erase_failure;
static uint8_t probe_pause;
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
    if (erase_failure != 0U) { return 0U; }
    if (probe_pause != 0U)
    {
        EnvAlarm_Sample sample = {250U,600U,1U};
        probe_pause=0U; ticks=100000U;
        HistoryBuffer_Poll(ticks,&sample,0U);
    }
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
    tx_failure = erase_failure = probe_pause = 0U;
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


static void confirm_clear(void) { start("log clear"); output[0]='\0'; start("log clear confirm"); }
uint32_t RunMaintenanceTests(void)
{
    History_Info info;
    History_Record record;
    HistoryBuffer_Stats stats;
    EnvAlarm_Config config = {350U,330U,800U,750U};
    EnvAlarm_Config loaded;
    uint32_t index;
    failures=0U;

    reset_fifo_scenario(); seed(3U); start("log clear");
    CHECK(0, request_ready==0U && strstr(output,"CONFIRM log clear confirm")!=NULL);
    CHECK(0, HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_OK && record.sequence==3U);
    output[0]='\0'; start("log clear confirm");
    CHECK(1, request_ready!=0U && output[0]=='\0' && ProjectCommands_TransferActive()!=0U);
    CHECK(1, pump(10U) && strstr(output,"OK log clear; next=4 discarded=0; SETTINGS_KEPT")!=NULL &&
             HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);

    reset_fifo_scenario(); start("log clear confirm");
    CHECK(2, request_ready==0U && strstr(output,"ERR CLEAR_CONFIRM")!=NULL);
    start("log clear"); ticks=LOG_CLEAR_CONFIRM_WINDOW_MS; start("log clear confirm");
    CHECK(3, request_ready==0U && strstr(output,"ERR CLEAR_CONFIRM")!=NULL);
    output[0]='\0'; ticks=0xFFFFFF00UL; start("log clear"); ticks=500U; start("log clear confirm");
    CHECK(4, request_ready!=0U && pump(10U));

    reset_fifo_scenario(); start("log clear"); start("diag"); output[0]='\0'; start("log clear confirm");
    CHECK(5, request_ready==0U && strstr(output,"ERR CLEAR_CONFIRM")!=NULL && diagnostic_calls>0U);
    start("log clear"); start("log cancel"); output[0]='\0'; start("log clear confirm");
    CHECK(5, request_ready==0U && strstr(output,"ERR CLEAR_CONFIRM")!=NULL);

    reset_fifo_scenario(); seed(3U); (void)SettingsStore_Save(&config);
    for(index=1U;index<=HISTORY_RECORD_QUEUE_LENGTH;++index) { produce_record(index*10U,260U); }
    confirm_clear();
    CHECK(6, pump(10U) && strstr(output,"discarded=8")!=NULL &&
             HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);
    HistoryBuffer_GetStats(&stats);
    CHECK(6, stats.used==0U && stats.discarded_clear==8U && stats.paused==0U);
    CHECK(7, SettingsStore_Load(&loaded)==SETTINGS_STORE_OK && loaded.temperature_on_x10==350U);
    produce_record(90U,270U); StorageService_Poll();
    CHECK(8, HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_OK && record.sequence==4U &&
             record.value.time.uptime_seconds==90U && HistoryStore_ReadRecent(1U,&record)==HISTORY_READ_EMPTY);
    output[0]='\0'; start("log status"); StorageService_Poll(); drain_replies();
    CHECK(9, strstr(output,"LOG_MAINT paused=0 discarded_clear=8 paused_periods=0")!=NULL);

    reset_fifo_scenario(); seed(3U); produce_record(10U,260U); probe_pause=1U;
    confirm_clear(); CHECK(10, pump(10U)); HistoryBuffer_GetStats(&stats);
    CHECK(10, stats.paused_periods==9U && stats.discarded_clear==1U && stats.accepted==1U &&
              stats.missed_periods==0U && stats.used==0U && stats.paused==0U);

    reset_fifo_scenario(); seed(3U); produce_record(10U,260U); erase_failure=1U;
    confirm_clear(); CHECK(11, pump(10U)); HistoryBuffer_GetStats(&stats); HistoryStore_GetInfo(&info);
    CHECK(11, strstr(output,"ERR LOG_CLEAR_IO")!=NULL && strstr(output,"OK log clear")==NULL &&
              stats.used==1U && stats.discarded_clear==0U && stats.paused==0U && info.ready==0U);

    reset_fifo_scenario(); seed(3U); start("log export"); StorageService_Poll();
    request_mailbox.kind=STORAGE_REQUEST_CLEAR; request_mailbox.request_id=999U; request_ready=1U;
    CHECK(12, pump(50U) && strstr(output,"CSV_END rows=3 skipped=0 state=OK")!=NULL &&
              HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_OK && record.sequence==3U);

    reset_fifo_scenario(); SerialCommand_Init(); SerialCommand_BindAlarmQueue((void *)6); control_sends=0U;
    feed("set temp 30\r"); feed("log clear\r");
    CHECK(13, control_sends==1U && request_ready==0U && strstr(output,"ERR BUSY")!=NULL);

    reset_fifo_scenario(); seed(3U); SerialCommand_Init();
    feed("LoG ClEaR\r"); output[0]='\0'; feed("LoG ClEaR CoNfIrM\r"); feed("log cancel\r");
    CHECK(14, pump(10U) && strstr(output,"OK log clear")!=NULL &&
              strstr(output,"ERR TRANSFER_BUSY")>strstr(output,"OK log clear"));

    reset_fifo_scenario(); SerialCommand_Init(); diagnostic_calls=0U;
    feed("DiAg\r"); feed("diag extra\r");
    CHECK(15, diagnostic_calls==1U && request_ready==0U && strstr(output,"ERR UNKNOWN_COMMAND")!=NULL);

    reset_fifo_scenario(); confirm_clear(); CHECK(16,pump(10U));
    output[0]='\0'; start("log clear confirm");
    CHECK(16, request_ready==0U && strstr(output,"ERR CLEAR_CONFIRM")!=NULL);

    reset_fifo_scenario(); start("flash info"); start("log clear");
    CHECK(17,strstr(output,"ERR STORAGE_BUSY")!=NULL); StorageService_Poll(); drain_replies();
    output[0]='\0'; start("log clear confirm");
    CHECK(17, request_ready==0U && strstr(output,"ERR CLEAR_CONFIRM")!=NULL);
    return failures;
}
