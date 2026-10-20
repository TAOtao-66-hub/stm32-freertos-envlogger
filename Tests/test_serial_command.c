#include "serial_command.h"
#include "serial_port.h"
#include "project_config.h"
#include <string.h>

uint8_t ProjectCommands_TransferActive(void) { return 0U; }
void ProjectCommands_DeferNotice(void) { }

HAL_StatusTypeDef ProjectCommands_Execute(const char *command, uint8_t *handled)
{
    (void)command;
    *handled = 0U; /* New service commands have separate transport-boundary tests. */
    return HAL_OK;
}

/* Real parser/report C code; only the UART transport boundary is replaced. */
static char sent_text[1024];
static uint16_t sent_count;
static uint32_t write_calls;
static uint32_t failures;
static HAL_StatusTypeDef next_tx_status;
static TestByteQueue alarm_queue;
static EnvAlarm_Request queued_request;
static uint8_t request_queued;
static uint32_t request_sends;

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks)
{
    if ((queue != &alarm_queue) || (wait_ticks != 0U) || (request_queued != 0U))
    {
        return pdFAIL;
    }
    queued_request = *(const EnvAlarm_Request *)item;
    request_queued = 1U;
    ++request_sends;
    return pdPASS;
}

#define CHECK(group, condition) \
    do { if (!(condition)) { failures |= (1UL << (group)); } } while (0)

HAL_StatusTypeDef SerialPort_Write(const uint8_t *data, uint16_t length)
{
    HAL_StatusTypeDef status = next_tx_status;

    next_tx_status = HAL_OK;
    ++write_calls;
    if ((data == NULL) || (length > SERIAL_TX_MAX_BYTES) ||
        ((uint32_t)sent_count + length >= sizeof(sent_text)))
    {
        return HAL_ERROR;
    }
    if (status == HAL_OK)
    {
        (void)memcpy(&sent_text[sent_count], data, length);
        sent_count += length;
        sent_text[sent_count] = '\0';
    }
    return status;
}

HAL_StatusTypeDef SerialPort_WriteString(const char *text)
{
    if (text == NULL)
    {
        return HAL_ERROR;
    }
    return SerialPort_Write((const uint8_t *)text, (uint16_t)strlen(text));
}

static void clear_output(void)
{
    sent_count = 0U;
    sent_text[0] = '\0';
    write_calls = 0U;
    next_tx_status = HAL_OK;
}

static void reset_parser(void)
{
    SerialCommand_Init();
    clear_output();
    request_queued = 0U;
    request_sends = 0U;
}

static HAL_StatusTypeDef feed(const char *text, uint32_t now_ms)
{
    HAL_StatusTypeDef status = HAL_OK;

    while (*text != '\0')
    {
        status = SerialCommand_ProcessByte((uint8_t)*text, now_ms);
        ++text;
    }
    return status;
}

static void expect(uint8_t group, const char *text, uint32_t calls)
{
    CHECK(group, sent_count == strlen(text));
    CHECK(group, strcmp(sent_text, text) == 0);
    CHECK(group, write_calls == calls);
}

uint32_t RunSerialCommandTests(void)
{
    EnvAlarm_Sample sample;
    uint16_t index;
    EnvAlarm_State owner;
    EnvAlarm_Response response;
    uint32_t saved_id;
    const char *bad_combined[] = {
        "set temp 30 hum", "set temp hum 70", "set temp 30 temp 40",
        "set hum 70 hum 80", "set temp 30 hum 70 junk", "set temp 30 xxx 70",
        "set temp 30 hum -1", "set temp 30 hum 70.00", "set temp 30 hum .5",
        "set temp 30.00 hum 70", "set temp 30 hum 70 temp 40"
    };
    const char *malformed[] = {
        "set", "set temp", "set temp 30 extra", "set temp -1",
        "set temp +30", "set temp 30.", "set temp .5", "set temp 30.00",
        "set temp 1e2", "set temp 9999999999999999999", "set other 30",
        "set temp 30c", "set temp 6553.6", "set temp 6554"
    };

    reset_parser();
    CHECK(0, feed("GET", 0U) == HAL_OK);
    expect(0, "", 0U);
    CHECK(0, feed("\n", 0U) == HAL_OK);
    expect(0, "ERR NO_SAMPLE\r\n", 1U);

    reset_parser();
    sample.temperature_x10 = 256U;
    sample.humidity_x10 = 610U;
    sample.valid = 1U;
    SerialCommand_UpdateSample(&sample, 100U);
    sample.temperature_x10 = 999U; /* The cache must own a copy. */
    CHECK(1, feed("GET\r\n", 101U) == HAL_OK);
    expect(1, "get TEMP=25.6C HUM=61.0% SENSOR=OK\r\n", 1U);
    clear_output();
    sample.valid = 0U;
    SerialCommand_UpdateSample(&sample, 102U);
    (void)feed("GET\n", 103U);
    expect(1, "get TEMP=--.-C HUM=--.-% SENSOR=ERROR\r\n", 1U);

    reset_parser();
    (void)feed("HELP\r\n\n\r\n", 0U);
    expect(2, "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n", 1U);
    clear_output();
    (void)feed("HELP\rHELP\n", 0U);
    expect(2, "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n"
              "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n", 2U);

    reset_parser();
    (void)feed(" \t GET \t\n", 0U);
    expect(3, "ERR NO_SAMPLE\r\n", 1U);
    clear_output();
    (void)feed("gEt\nget extra\ngetx\n", 0U);
    expect(3, "ERR NO_SAMPLE\r\n"
              "ERR UNKNOWN_COMMAND; SEND help\r\n"
              "ERR UNKNOWN_COMMAND; SEND help\r\n", 3U);

    reset_parser();
    for (index = 0U; index < SERIAL_COMMAND_MAX_LENGTH + 1U; ++index)
    {
        (void)SerialCommand_ProcessByte('A', 0U);
    }
    (void)feed("GET\n", 0U); /* Overflow suffix must never execute as GET. */
    expect(4, "ERR LINE_TOO_LONG\r\n", 1U);
    clear_output();
    (void)feed("GET\n", 0U);
    expect(4, "ERR NO_SAMPLE\r\n", 1U);

    reset_parser();
    for (index = 0U; index < SERIAL_COMMAND_MAX_LENGTH; ++index)
    {
        (void)SerialCommand_ProcessByte('A', 0U);
    }
    (void)feed("\r\n", 0U);
    expect(5, "ERR UNKNOWN_COMMAND; SEND help\r\n", 1U);
    clear_output();
    (void)feed(" \t \n", 0U);
    expect(5, "", 0U);

    reset_parser();
    (void)feed("GE", 0U);
    (void)SerialCommand_ProcessByte(0U, 0U);
    (void)feed("T\n", 0U);
    expect(6, "ERR INVALID_CHAR\r\n", 1U);
    clear_output();
    (void)SerialCommand_ProcessByte(0x80U, 0U);
    (void)feed("GET\r\n", 0U);
    expect(6, "ERR INVALID_CHAR\r\n", 1U);
    clear_output();
    (void)feed("GET\n", 0U);
    expect(6, "ERR NO_SAMPLE\r\n", 1U);

    reset_parser();
    sample.temperature_x10 = 300U;
    sample.humidity_x10 = 700U;
    sample.valid = 1U;
    SerialCommand_UpdateSample(&sample, 10U);
    (void)feed("G", 10U);
    SerialCommand_DiscardLine(10U);
    (void)feed("ET\r\n", 10U);
    expect(7, "ERR RX_LOSS; RESEND COMMAND\r\n", 1U);
    clear_output();
    (void)feed("GET\n", 10U);
    expect(7, "get TEMP=30.0C HUM=70.0% SENSOR=OK\r\n", 1U);
    clear_output();
    SerialCommand_DiscardLine(10U);
    (void)feed("GET\n", 10U); /* Resync terminator, not a command. */
    expect(7, "ERR RX_LOSS; RESEND COMMAND\r\n", 1U);

    reset_parser();
    SerialCommand_UpdateSample(&sample, 100U);
    (void)feed("GET\n", 100U + ENV_ALARM_SAMPLE_TIMEOUT_MS - 1U);
    expect(8, "get TEMP=30.0C HUM=70.0% SENSOR=OK\r\n", 1U);
    clear_output();
    (void)feed("GET\n", 100U + ENV_ALARM_SAMPLE_TIMEOUT_MS);
    expect(8, "ERR SAMPLE_STALE\r\n", 1U);
    clear_output();
    SerialCommand_UpdateSample(&sample, 0xFFFFFFF0UL);
    (void)feed("GET\n", 0x00000010UL);
    expect(8, "get TEMP=30.0C HUM=70.0% SENSOR=OK\r\n", 1U);
    clear_output();
    (void)feed("GET\n", (uint32_t)(0xFFFFFFF0UL + ENV_ALARM_SAMPLE_TIMEOUT_MS));
    expect(8, "ERR SAMPLE_STALE\r\n", 1U);

    reset_parser();
    next_tx_status = HAL_TIMEOUT;
    CHECK(9, feed("HELP\n", 0U) == HAL_TIMEOUT);
    expect(9, "", 1U);
    clear_output();
    CHECK(9, feed("GET\n", 0U) == HAL_OK);
    expect(9, "ERR NO_SAMPLE\r\n", 1U);
    clear_output();
    next_tx_status = HAL_BUSY;
    CHECK(9, feed("GET\n", 0U) == HAL_BUSY);
    clear_output();
    (void)feed("HELP\n", 0U);
    expect(9, "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n", 1U);

    reset_parser();
    SerialCommand_UpdateSample(NULL, 0U);
    (void)feed("GET\nGET\r\n", 0U);
    expect(10, "ERR NO_SAMPLE\r\nERR NO_SAMPLE\r\n", 2U);
    clear_output();
    SerialCommand_UpdateSample(&sample, 1U);
    SerialCommand_Init();
    (void)feed("GET\n", 2U);
    expect(10, "ERR NO_SAMPLE\r\n", 1U);
    /* Idle framing: plain lowercase get, exactly one response at the boundary. */
    reset_parser();
    sample.temperature_x10 = 256U;
    sample.humidity_x10 = 610U;
    sample.valid = 1U;
    SerialCommand_UpdateSample(&sample, 50U);
    (void)feed("get", 50U);
    CHECK(11, SerialCommand_Poll(50U + SERIAL_COMMAND_IDLE_MS - 1U) == HAL_OK);
    expect(11, "", 0U);
    CHECK(11, SerialCommand_Poll(50U + SERIAL_COMMAND_IDLE_MS) == HAL_OK);
    expect(11, "get TEMP=25.6C HUM=61.0% SENSOR=OK\r\n", 1U);
    (void)SerialCommand_Poll(50U + SERIAL_COMMAND_IDLE_MS + 1U);
    expect(11, "get TEMP=25.6C HUM=61.0% SENSOR=OK\r\n", 1U);

    reset_parser();
    (void)feed("help", 0U);
    (void)SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS);
    (void)feed("\r\n", SERIAL_COMMAND_IDLE_MS + 1U);
    expect(12, "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n", 1U);
    clear_output();
    (void)feed("HeLp", 500U);
    (void)SerialCommand_Poll(500U + SERIAL_COMMAND_IDLE_MS);
    expect(12, "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n", 1U);

    reset_parser();
    (void)feed("g", 0U);
    (void)SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS - 1U);
    (void)feed("e", SERIAL_COMMAND_IDLE_MS - 1U);
    (void)SerialCommand_Poll(2U * (SERIAL_COMMAND_IDLE_MS - 1U));
    (void)feed("t", 2U * (SERIAL_COMMAND_IDLE_MS - 1U));
    (void)SerialCommand_Poll(3U * SERIAL_COMMAND_IDLE_MS - 3U);
    expect(13, "", 0U);
    (void)SerialCommand_Poll(3U * SERIAL_COMMAND_IDLE_MS - 2U);
    expect(13, "ERR NO_SAMPLE\r\n", 1U);

    reset_parser();
    (void)feed("getx", 0U);
    expect(14, "", 0U);
    (void)SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS);
    expect(14, "ERR UNKNOWN_COMMAND; SEND help\r\n", 1U);
    clear_output();
    (void)feed("gethelp", 200U);
    (void)SerialCommand_Poll(200U + SERIAL_COMMAND_IDLE_MS);
    expect(14, "ERR UNKNOWN_COMMAND; SEND help\r\n", 1U);
    clear_output();
    (void)feed("get", 400U);
    (void)SerialCommand_Poll(400U + SERIAL_COMMAND_IDLE_MS);
    (void)feed("get", 600U);
    (void)SerialCommand_Poll(600U + SERIAL_COMMAND_IDLE_MS);
    expect(14, "ERR NO_SAMPLE\r\nERR NO_SAMPLE\r\n", 2U);

    reset_parser();
    for (index = 0U; index < SERIAL_COMMAND_MAX_LENGTH + 1U; ++index)
    {
        (void)SerialCommand_ProcessByte('a', 0U);
    }
    (void)SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS);
    expect(15, "ERR LINE_TOO_LONG\r\n", 1U);
    clear_output();
    (void)feed("get", 200U);
    (void)SerialCommand_Poll(200U + SERIAL_COMMAND_IDLE_MS);
    expect(15, "ERR NO_SAMPLE\r\n", 1U);
    clear_output();
    (void)SerialCommand_ProcessByte(0U, 400U);
    (void)SerialCommand_Poll(400U + SERIAL_COMMAND_IDLE_MS);
    expect(15, "ERR INVALID_CHAR\r\n", 1U);
    clear_output();
    SerialCommand_DiscardLine(600U);
    (void)SerialCommand_Poll(600U + SERIAL_COMMAND_IDLE_MS - 1U);
    expect(15, "", 0U);
    (void)SerialCommand_Poll(600U + SERIAL_COMMAND_IDLE_MS);
    expect(15, "ERR RX_LOSS; RESEND COMMAND\r\n", 1U);
    clear_output();
    (void)feed("get", 800U);
    (void)SerialCommand_Poll(800U + SERIAL_COMMAND_IDLE_MS);
    expect(15, "ERR NO_SAMPLE\r\n", 1U);

    reset_parser();
    (void)feed("help", 0xFFFFFFF0UL);
    (void)SerialCommand_Poll((uint32_t)(0xFFFFFFF0UL + SERIAL_COMMAND_IDLE_MS - 1U));
    expect(16, "", 0U);
    (void)SerialCommand_Poll((uint32_t)(0xFFFFFFF0UL + SERIAL_COMMAND_IDLE_MS));
    expect(16, "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n", 1U);

    reset_parser();
    (void)feed("help", 0U);
    next_tx_status = HAL_TIMEOUT;
    CHECK(17, SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS) == HAL_TIMEOUT);
    expect(17, "", 1U);
    clear_output();
    (void)feed("get", 200U);
    CHECK(17, SerialCommand_Poll(200U + SERIAL_COMMAND_IDLE_MS) == HAL_OK);
    expect(17, "ERR NO_SAMPLE\r\n", 1U);
    /* SET sends no success until the actual alarm owner has applied it. */
    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    sample.temperature_x10 = 350U;
    sample.humidity_x10 = 500U;
    sample.valid = 1U;
    (void)feed("set temp 30.0", 20U);
    (void)SerialCommand_Poll(20U + SERIAL_COMMAND_IDLE_MS - 1U);
    CHECK(18, request_sends == 0U);
    (void)SerialCommand_Poll(20U + SERIAL_COMMAND_IDLE_MS);
    CHECK(18, request_queued == 1U && queued_request.temperature_x10 == 300U);
    CHECK(18, queued_request.kind == ENV_ALARM_REQUEST_SET_TEMPERATURE);
    expect(18, "", 0U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, &sample, &response);
    CHECK(18, owner.flags == ENV_ALARM_FLAG_TEMPERATURE);
    CHECK(18, owner.config.temperature_off_x10 == 280U);
    CHECK(18, SerialCommand_HandleAlarmResponse(&response) == HAL_OK);
    expect(18, "OK set temp on=30.0C off=28.0C\r\n", 1U);
    (void)SerialCommand_HandleAlarmResponse(&response); /* No duplicate ACK. */
    expect(18, "OK set temp on=30.0C off=28.0C\r\n", 1U);

    for (index = 0U; index < sizeof(malformed) / sizeof(malformed[0]); ++index)
    {
        reset_parser();
        SerialCommand_BindAlarmQueue(&alarm_queue);
        (void)feed(malformed[index], 0U);
        (void)SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS);
        CHECK(19, request_sends == 0U);
        expect(19, "ERR FORMAT; USE set temp 30.0 [hum 70.0] OR set hum 70.0\r\n", 1U);
    }

    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    (void)feed("set temp 1.9\n", 0U);
    expect(20, "", 0U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    CHECK(20, response.result == ENV_ALARM_REQUEST_RANGE_ERROR);
    CHECK(20, owner.config.temperature_on_x10 == ENV_ALARM_TEMP_ON_X10);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(20, "ERR RANGE temp=2.0..50.0C\r\n", 1U);
    clear_output();
    (void)feed("set hum 100.1\n", 1U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(20, "ERR RANGE hum=5.0..100.0%\r\n", 1U);
    CHECK(20, owner.flags == 0U && owner.config.humidity_on_x10 == ENV_ALARM_HUM_ON_X10);

    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    (void)feed("set temp 31\n", 0U);
    saved_id = queued_request.request_id;
    (void)feed("set hum 80\n", 1U);
    expect(21, "ERR BUSY; WAIT FOR RESULT\r\n", 1U);
    CHECK(21, request_sends == 1U && queued_request.request_id == saved_id);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    clear_output();
    ++response.request_id;
    (void)SerialCommand_HandleAlarmResponse(&response);
    response.request_id = saved_id;
    response.kind = ENV_ALARM_REQUEST_SET_HUMIDITY;
    (void)SerialCommand_HandleAlarmResponse(&response);
    (void)SerialCommand_HandleAlarmResponse(NULL);
    expect(21, "", 0U);
    (void)feed("help\n", 2U);
    expect(21, "commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n", 1U);
    clear_output();
    response.kind = ENV_ALARM_REQUEST_SET_TEMPERATURE;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(21, "OK set temp on=31.0C off=29.0C\r\n", 1U);
    clear_output();
    (void)feed("set hum 80\n", 3U);
    CHECK(21, request_sends == 2U && queued_request.request_id != saved_id);
    expect(21, "", 0U);

    reset_parser();
    (void)feed("set temp 30\n", 0U);
    expect(22, "ERR CONTROL_NOT_READY\r\n", 1U);
    clear_output();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    request_queued = 1U; /* Simulate a full request mailbox. */
    (void)feed("set temp 30\n", 1U);
    expect(22, "ERR BUSY; RETRY\r\n", 1U);
    CHECK(22, request_sends == 0U);
    clear_output();
    request_queued = 0U;
    (void)feed("set temp 30\n", 2U);
    CHECK(22, request_sends == 1U);
    expect(22, "", 0U);
    EnvAlarm_Init(&owner);
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    next_tx_status = HAL_TIMEOUT;
    CHECK(22, SerialCommand_HandleAlarmResponse(&response) == HAL_TIMEOUT);
    clear_output();
    request_queued = 0U;
    (void)feed("set hum 80\n", 3U); /* Failed ACK must not lock future settings. */
    CHECK(22, request_sends == 2U);
    expect(22, "", 0U);

    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    (void)feed(" \t SeT\tHuM 100.0 \t\r\n", 0U);
    CHECK(23, queued_request.humidity_x10 == 1000U && request_sends == 1U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(23, "OK set hum on=100.0% off=95.0%\r\n", 1U);
    clear_output();
    (void)feed("set temp 6553.5\n", 1U); /* Valid uint16 representation, outside sensor range. */
    CHECK(23, queued_request.temperature_x10 == 65535U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(23, "ERR RANGE temp=2.0..50.0C\r\n", 1U);
    /* One compound command creates exactly one request and one applied ACK. */
    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    sample.temperature_x10 = 360U;
    sample.humidity_x10 = 850U;
    sample.valid = 1U;
    (void)feed("set temp 35.0 hum 80.0", 5U);
    (void)SerialCommand_Poll(5U + SERIAL_COMMAND_IDLE_MS - 1U);
    CHECK(24, request_sends == 0U);
    (void)SerialCommand_Poll(5U + SERIAL_COMMAND_IDLE_MS);
    CHECK(24, request_sends == 1U && request_queued == 1U);
    CHECK(24, queued_request.kind == ENV_ALARM_REQUEST_SET_BOTH);
    CHECK(24, queued_request.temperature_x10 == 350U && queued_request.humidity_x10 == 800U);
    CHECK(24, owner.config.temperature_on_x10 == ENV_ALARM_TEMP_ON_X10);
    expect(24, "", 0U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, &sample, &response);
    CHECK(24, owner.config.temperature_on_x10 == 350U && owner.config.humidity_on_x10 == 800U);
    CHECK(24, owner.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY));
    CHECK(24, SerialCommand_HandleAlarmResponse(&response) == HAL_OK);
    expect(24, "OK set temp on=35.0C off=33.0C; hum on=80.0% off=75.0%\r\n", 1U);
    (void)feed("\r\n", 200U);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(24, "OK set temp on=35.0C off=33.0C; hum on=80.0% off=75.0%\r\n", 1U);

    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    (void)feed(" SeT HuM 100 TeMp 50\r\n", 0U);
    CHECK(25, request_sends == 1U && queued_request.kind == ENV_ALARM_REQUEST_SET_BOTH);
    CHECK(25, queued_request.temperature_x10 == 500U && queued_request.humidity_x10 == 1000U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(25, "OK set temp on=50.0C off=48.0C; hum on=100.0% off=95.0%\r\n", 1U);
    clear_output();
    (void)feed("set temp 2 hum 5\n", 1U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(25, "OK set temp on=2.0C off=0.0C; hum on=5.0% off=0.0%\r\n", 1U);
    CHECK(25, owner.flags == 0U);

    for (index = 0U; index < sizeof(bad_combined) / sizeof(bad_combined[0]); ++index)
    {
        reset_parser();
        SerialCommand_BindAlarmQueue(&alarm_queue);
        (void)feed(bad_combined[index], 0U);
        (void)SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS);
        CHECK(26, request_sends == 0U);
        expect(26, "ERR FORMAT; USE set temp 30.0 [hum 70.0] OR set hum 70.0\r\n", 1U);
    }

    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    owner.flags = ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY;
    owner.muted = 1U;
    sample.temperature_x10 = 0U;
    sample.humidity_x10 = 0U;
    sample.valid = 1U;
    (void)feed("set temp 40 hum 100.1\n", 0U);
    expect(27, "", 0U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, &sample, &response);
    CHECK(27, response.result == ENV_ALARM_REQUEST_RANGE_ERROR);
    CHECK(27, owner.config.temperature_on_x10 == ENV_ALARM_TEMP_ON_X10);
    CHECK(27, owner.config.humidity_on_x10 == ENV_ALARM_HUM_ON_X10);
    CHECK(27, owner.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY) && owner.muted == 1U);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(27, "ERR RANGE temp=2.0..50.0C; hum=5.0..100.0%; UNCHANGED\r\n", 1U);
    clear_output();
    (void)feed("set temp 1.9 hum 80\n", 1U);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, &sample, &response);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(27, "ERR RANGE temp=2.0..50.0C; hum=5.0..100.0%; UNCHANGED\r\n", 1U);
    CHECK(27, owner.config.temperature_off_x10 == ENV_ALARM_TEMP_OFF_X10);
    CHECK(27, owner.config.humidity_off_x10 == ENV_ALARM_HUM_OFF_X10);

    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    EnvAlarm_Init(&owner);
    (void)feed("set temp 32.5 hum 75.5\n", 0U);
    saved_id = queued_request.request_id;
    (void)feed("set temp 35 hum 80\n", 0U);
    expect(28, "ERR BUSY; WAIT FOR RESULT\r\n", 1U);
    CHECK(28, request_sends == 1U && queued_request.request_id == saved_id);
    clear_output();
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    response.kind = ENV_ALARM_REQUEST_SET_TEMPERATURE;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(28, "", 0U);
    response.kind = ENV_ALARM_REQUEST_SET_BOTH;
    CHECK(28, SerialCommand_HandleAlarmResponse(&response) == HAL_OK);
    expect(28, "OK set temp on=32.5C off=30.5C; hum on=75.5% off=70.5%\r\n", 1U);
    clear_output();
    (void)feed("set temp 40\n", 1U);
    CHECK(28, queued_request.kind == ENV_ALARM_REQUEST_SET_TEMPERATURE);
    request_queued = 0U;
    EnvAlarm_ApplyRequest(&owner, &queued_request, NULL, &response);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(28, "OK set temp on=40.0C off=38.0C\r\n", 1U);
    CHECK(28, owner.config.humidity_on_x10 == 755U && owner.config.humidity_off_x10 == 705U);
    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    (void)feed("save", 0U);
    (void)SerialCommand_Poll(SERIAL_COMMAND_IDLE_MS);
    CHECK(29, request_sends == 1U && queued_request.kind == ENV_ALARM_REQUEST_SAVE);
    expect(29, "", 0U);
    (void)feed("set temp 40\n", 200U);
    expect(29, "ERR BUSY; WAIT FOR RESULT\r\n", 1U);
    clear_output();
    response.request_id = queued_request.request_id;
    response.kind = ENV_ALARM_REQUEST_SAVE;
    response.result = ENV_ALARM_REQUEST_OK;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(29, "OK save; STORED\r\n", 1U);
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(29, "OK save; STORED\r\n", 1U);
    clear_output(); request_queued = 0U;
    (void)feed("SAVE\r\n", 300U);
    response.request_id = queued_request.request_id;
    response.result = ENV_ALARM_REQUEST_SAVE_UNCHANGED;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(29, "OK save; UNCHANGED\r\n", 1U);

    reset_parser();
    (void)feed("save\n", 0U);
    expect(30, "ERR CONTROL_NOT_READY\r\n", 1U);
    clear_output(); SerialCommand_BindAlarmQueue(&alarm_queue);
    (void)feed("save extra\n", 1U);
    expect(30, "ERR UNKNOWN_COMMAND; SEND help\r\n", 1U);
    clear_output();
    (void)feed("save\n", 2U);
    response.request_id = queued_request.request_id;
    response.kind = ENV_ALARM_REQUEST_SAVE;
    response.result = ENV_ALARM_REQUEST_STORAGE_ERROR;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(30, "ERR SAVE; RETRY\r\n", 1U);
    clear_output(); request_queued = 0U;
    (void)feed("save\n", 3U);
    CHECK(30, request_sends == 2U);
    response.request_id = queued_request.request_id;
    response.result = ENV_ALARM_REQUEST_OK;
    next_tx_status = HAL_TIMEOUT;
    CHECK(30, SerialCommand_HandleAlarmResponse(&response) == HAL_TIMEOUT);
    clear_output(); request_queued = 0U;
    (void)feed("save\n", 4U);
    CHECK(30, request_sends == 3U);
    reset_parser();
    SerialCommand_BindAlarmQueue(&alarm_queue);
    (void)feed("pot temp\n", 0U);
    CHECK(31, request_sends == 1U && queued_request.kind == ENV_ALARM_REQUEST_POT_TEMP);
    expect(31, "", 0U);
    response.request_id = queued_request.request_id;
    response.kind = ENV_ALARM_REQUEST_POT_TEMP;
    response.result = ENV_ALARM_REQUEST_OK;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(31, "OK pot temp\r\n", 1U);
    clear_output(); request_queued = 0U;
    (void)feed("set temp 35 hum 80\n", 1U);
    response.request_id = queued_request.request_id;
    response.kind = ENV_ALARM_REQUEST_SET_BOTH;
    response.result = ENV_ALARM_REQUEST_LOCAL_CONTROL;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(31, "ERR LOCAL_CONTROL; SEND pot off BEFORE REMOTE SET\r\n", 1U);
    clear_output(); request_queued = 0U;
    (void)feed("pot off\n", 2U);
    CHECK(31, queued_request.kind == ENV_ALARM_REQUEST_POT_OFF);
    response.request_id = queued_request.request_id;
    response.kind = ENV_ALARM_REQUEST_POT_OFF;
    response.result = ENV_ALARM_REQUEST_OK;
    (void)SerialCommand_HandleAlarmResponse(&response);
    expect(31, "OK pot off\r\n", 1U);
    return failures;
}
