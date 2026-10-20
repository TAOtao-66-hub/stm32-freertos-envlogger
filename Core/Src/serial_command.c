#include "serial_command.h"
#include "serial_port.h"
#include "project_commands.h"
#include "serial_report.h"
#include "project_config.h"
#include <string.h>
#include <stdio.h>

/* Private types ----------------------------------------------------------- */
typedef enum
{
    SERIAL_COMMAND_DISCARD_NONE = 0,
    SERIAL_COMMAND_DISCARD_TOO_LONG,
    SERIAL_COMMAND_DISCARD_INVALID_CHAR,
    SERIAL_COMMAND_DISCARD_RX_LOSS
} SerialCommand_DiscardReason;

typedef struct
{
    char line[SERIAL_COMMAND_MAX_LENGTH + 1U];
    uint16_t length;
    SerialCommand_DiscardReason discard_reason;
    EnvAlarm_Sample latest_sample;
    uint32_t last_sample_ms;
    uint32_t last_byte_ms;
    uint8_t has_sample;
    QueueHandle_t alarm_request_queue;
    uint32_t next_request_id;
    uint32_t pending_request_id;
    EnvAlarm_RequestKind pending_kind;
    uint8_t request_pending;
} SerialCommand_Context;

/* Private state: only serialTask reads or writes this context. ------------- */
static SerialCommand_Context s_serial_command;

/* Private functions ------------------------------------------------------- */
static uint8_t is_space(char character)
{
    return ((character == ' ') || (character == '\t')) ? 1U : 0U;
}

/* Parse integer or exactly one decimal digit into tenths, without float.
 * Bound each step before any 16-bit conversion; reject signs and suffixes. */
static uint8_t parse_value_x10(const char *text, uint16_t *value)
{
    uint32_t whole = 0U;
    uint32_t scaled;

    if ((*text < '0') || (*text > '9'))
    {
        return 0U;
    }
    while ((*text >= '0') && (*text <= '9'))
    {
        whole = whole * 10U + (uint32_t)(*text - '0');
        if (whole > 6553U)
        {
            return 0U;
        }
        ++text;
    }
    scaled = whole * 10U;
    if (*text == '.')
    {
        ++text;
        if ((*text < '0') || (*text > '9'))
        {
            return 0U;
        }
        scaled += (uint32_t)(*text - '0');
        ++text;
    }
    if ((*text != '\0') || (scaled > 65535U))
    {
        return 0U;
    }
    *value = (uint16_t)scaled;
    return 1U;
}

static char *next_token(char **cursor)
{
    char *token;

    while (is_space(**cursor) != 0U)
    {
        ++(*cursor);
    }
    if (**cursor == '\0')
    {
        return NULL;
    }
    token = *cursor;
    while ((**cursor != '\0') && (is_space(**cursor) == 0U))
    {
        ++(*cursor);
    }
    if (**cursor != '\0')
    {
        **cursor = '\0';
        ++(*cursor);
    }
    return token;
}

/* Parse one or two distinct field/value pairs. No request is submitted until
 * the entire text is valid; the alarm owner performs numeric range validation. */
static uint8_t parse_setting(char *command, EnvAlarm_Request *request)
{
    char *cursor = command;
    char *field;
    char *value;
    uint8_t has_temperature = 0U;
    uint8_t has_humidity = 0U;

    request->temperature_x10 = 0U;
    request->humidity_x10 = 0U;
    (void)next_token(&cursor); /* set */
    while ((field = next_token(&cursor)) != NULL)
    {
        value = next_token(&cursor);
        if (value == NULL)
        {
            return 0U;
        }
        if (strcmp(field, "temp") == 0)
        {
            if ((has_temperature != 0U) ||
                (parse_value_x10(value, &request->temperature_x10) == 0U))
            {
                return 0U;
            }
            has_temperature = 1U;
        }
        else if (strcmp(field, "hum") == 0)
        {
            if ((has_humidity != 0U) ||
                (parse_value_x10(value, &request->humidity_x10) == 0U))
            {
                return 0U;
            }
            has_humidity = 1U;
        }
        else
        {
            return 0U;
        }
    }
    if ((has_temperature == 0U) && (has_humidity == 0U))
    {
        return 0U;
    }
    if ((has_temperature != 0U) && (has_humidity != 0U))
    {
        request->kind = ENV_ALARM_REQUEST_SET_BOTH;
    }
    else
    {
        request->kind = (has_temperature != 0U) ? ENV_ALARM_REQUEST_SET_TEMPERATURE :
                                                ENV_ALARM_REQUEST_SET_HUMIDITY;
    }
    return 1U;
}

static HAL_StatusTypeDef submit_request(EnvAlarm_Request *request)
{
    if (s_serial_command.alarm_request_queue == NULL)
    {
        return SerialPort_WriteString("ERR CONTROL_NOT_READY\r\n");
    }
    if (s_serial_command.request_pending != 0U)
    {
        return SerialPort_WriteString("ERR BUSY; WAIT FOR RESULT\r\n");
    }
    ++s_serial_command.next_request_id;
    if (s_serial_command.next_request_id == 0U)
    {
        ++s_serial_command.next_request_id;
    }
    request->request_id = s_serial_command.next_request_id;
    if (xQueueSend(s_serial_command.alarm_request_queue, request, 0U) != pdPASS)
    {
        return SerialPort_WriteString("ERR BUSY; RETRY\r\n");
    }
    s_serial_command.pending_request_id = request->request_id;
    s_serial_command.pending_kind = request->kind;
    s_serial_command.request_pending = 1U;
    return HAL_OK; /* The alarm owner's response, not enqueueing, triggers OK. */
}

static HAL_StatusTypeDef submit_setting(char *command)
{
    EnvAlarm_Request request;

    if (parse_setting(command, &request) == 0U)
    {
        return SerialPort_WriteString("ERR FORMAT; USE set temp 30.0 [hum 70.0] OR set hum 70.0\r\n");
    }
    return submit_request(&request);
}

static HAL_StatusTypeDef submit_save(void)
{
    EnvAlarm_Request request;

    request.kind = ENV_ALARM_REQUEST_SAVE;
    request.temperature_x10 = 0U;
    request.humidity_x10 = 0U;
    return submit_request(&request);
}

static HAL_StatusTypeDef execute_line(uint32_t now_ms)
{
    char *command = s_serial_command.line;
    uint16_t end = s_serial_command.length;
    uint8_t handled;
    HAL_StatusTypeDef extension_status;

    /* Trim only the line edges; get extra is still an unknown command. */
    while ((end > 0U) && (is_space(command[end - 1U]) != 0U))
    {
        --end;
    }
    command[end] = '\0';
    while (is_space(*command) != 0U)
    {
        ++command;
    }

    /* Clear the parser before TX, even if the response later times out. */
    s_serial_command.length = 0U;
    if (*command == '\0')
    {
        return HAL_OK;
    }
    /* A prior set/save reply must finish before opening a framed stream. */
    if ((s_serial_command.request_pending != 0U) &&
        ((strcmp(command, "log export") == 0) || (strncmp(command, "log page ", 9U) == 0) ||
         (strcmp(command, "log clear") == 0) || (strcmp(command, "log clear confirm") == 0)))
    {
        return SerialPort_WriteString("ERR BUSY; WAIT FOR RESULT\r\n");
    }
    extension_status = ProjectCommands_Execute(command, &handled);
    if (handled != 0U) { return extension_status; }
    if ((strcmp(command, "pot off") == 0) || (strcmp(command, "pot temp") == 0) || (strcmp(command, "pot hum") == 0))
    {
        EnvAlarm_Request request;
        request.request_id = 0U;
        request.temperature_x10 = 0U;
        request.humidity_x10 = 0U;
        request.kind = (strcmp(command, "pot off") == 0) ? ENV_ALARM_REQUEST_POT_OFF :
                       ((strcmp(command, "pot temp") == 0) ? ENV_ALARM_REQUEST_POT_TEMP : ENV_ALARM_REQUEST_POT_HUM);
        return submit_request(&request);
    }
    if (strcmp(command, "help") == 0)
    {
        return SerialPort_WriteString("commands: get | help | save | set temp <C> [hum <%>] | set hum <%>\r\n");
    }
    if (strcmp(command, "save") == 0)
    {
        return submit_save();
    }
    if (strcmp(command, "get") == 0)
    {
        if (s_serial_command.has_sample == 0U)
        {
            return SerialPort_WriteString("ERR NO_SAMPLE\r\n");
        }
        /* Same timeout as the alarm: do not present a stopped stream as fresh.
         * Age starts when serialTask receives the acquisition snapshot. */
        if ((uint32_t)(now_ms - s_serial_command.last_sample_ms) >= ENV_ALARM_SAMPLE_TIMEOUT_MS)
        {
            return SerialPort_WriteString("ERR SAMPLE_STALE\r\n");
        }
        return SerialReport_SendQuery(&s_serial_command.latest_sample);
    }
    if ((strcmp(command, "set") == 0) ||
        ((strncmp(command, "set", 3U) == 0) && (is_space(command[3]) != 0U)))
    {
        return submit_setting(command);
    }
    return SerialPort_WriteString("ERR UNKNOWN_COMMAND; SEND help\r\n");
}

static HAL_StatusTypeDef finish_line(uint32_t now_ms)
{
    SerialCommand_DiscardReason reason = s_serial_command.discard_reason;

    if (reason == SERIAL_COMMAND_DISCARD_NONE)
    {
        return execute_line(now_ms);
    }

    /* Reject the whole command. Idle and explicit line endings both resync.
     * Never execute a suffix left after overflow or RX loss. */
    s_serial_command.length = 0U;
    s_serial_command.discard_reason = SERIAL_COMMAND_DISCARD_NONE;
    if (ProjectCommands_TransferActive() != 0U)
    {
        ProjectCommands_DeferNotice();
        return HAL_OK;
    }
    if (reason == SERIAL_COMMAND_DISCARD_TOO_LONG)
    {
        return SerialPort_WriteString("ERR LINE_TOO_LONG\r\n");
    }
    if (reason == SERIAL_COMMAND_DISCARD_INVALID_CHAR)
    {
        return SerialPort_WriteString("ERR INVALID_CHAR\r\n");
    }
    return SerialPort_WriteString("ERR RX_LOSS; RESEND COMMAND\r\n");
}

/* One reply describes both values returned by the alarm owner. */
static HAL_StatusTypeDef send_combined_response(const EnvAlarm_Response *response)
{
    char text[96];
    const EnvAlarm_Config *config = &response->config;
    int length;

    if (response->result == ENV_ALARM_REQUEST_OK)
    {
        length = snprintf(text, sizeof(text),
                          "OK set temp on=%u.%uC off=%u.%uC; hum on=%u.%u%% off=%u.%u%%\r\n",
                          (unsigned int)(config->temperature_on_x10 / 10U),
                          (unsigned int)(config->temperature_on_x10 % 10U),
                          (unsigned int)(config->temperature_off_x10 / 10U),
                          (unsigned int)(config->temperature_off_x10 % 10U),
                          (unsigned int)(config->humidity_on_x10 / 10U),
                          (unsigned int)(config->humidity_on_x10 % 10U),
                          (unsigned int)(config->humidity_off_x10 / 10U),
                          (unsigned int)(config->humidity_off_x10 % 10U));
    }
    else if (response->result == ENV_ALARM_REQUEST_RANGE_ERROR)
    {
        length = snprintf(text, sizeof(text),
                          "ERR RANGE temp=%u.%u..%u.%uC; hum=%u.%u..%u.%u%%; UNCHANGED\r\n",
                          (unsigned int)(ENV_ALARM_TEMP_HYSTERESIS_X10 / 10U),
                          (unsigned int)(ENV_ALARM_TEMP_HYSTERESIS_X10 % 10U),
                          (unsigned int)(ENV_ALARM_TEMP_SET_MAX_X10 / 10U),
                          (unsigned int)(ENV_ALARM_TEMP_SET_MAX_X10 % 10U),
                          (unsigned int)(ENV_ALARM_HUM_HYSTERESIS_X10 / 10U),
                          (unsigned int)(ENV_ALARM_HUM_HYSTERESIS_X10 % 10U),
                          (unsigned int)(ENV_ALARM_HUM_SET_MAX_X10 / 10U),
                          (unsigned int)(ENV_ALARM_HUM_SET_MAX_X10 % 10U));
    }
    else
    {
        return SerialPort_WriteString("ERR BAD_REQUEST\r\n");
    }
    if ((length < 0) || ((unsigned int)length >= sizeof(text)))
    {
        return HAL_ERROR;
    }
    return SerialPort_Write((const uint8_t *)text, (uint16_t)length);
}

/* Public functions -------------------------------------------------------- */
void SerialCommand_Init(void)
{
    (void)memset(&s_serial_command, 0, sizeof(s_serial_command));
}

void SerialCommand_BindAlarmQueue(QueueHandle_t request_queue)
{
    s_serial_command.alarm_request_queue = request_queue;
}

HAL_StatusTypeDef SerialCommand_HandleAlarmResponse(const EnvAlarm_Response *response)
{
    char text[96];
    const char *field;
    const char *unit;
    uint16_t on_value;
    uint16_t off_value;
    uint16_t minimum;
    uint16_t maximum;
    int length;

    if ((response == NULL) || (s_serial_command.request_pending == 0U) ||
        (response->request_id != s_serial_command.pending_request_id) ||
        (response->kind != s_serial_command.pending_kind))
    {
        return HAL_OK;
    }
    s_serial_command.request_pending = 0U;
    if (response->result == ENV_ALARM_REQUEST_LOCAL_CONTROL)
    { return SerialPort_WriteString("ERR LOCAL_CONTROL; SEND pot off BEFORE REMOTE SET\r\n"); }
    if ((response->kind >= ENV_ALARM_REQUEST_POT_OFF) && (response->kind <= ENV_ALARM_REQUEST_POT_HUM))
    {
        if (response->result != ENV_ALARM_REQUEST_OK) { return SerialPort_WriteString("ERR POT_NOT_READY\r\n"); }
        return SerialPort_WriteString((response->kind == ENV_ALARM_REQUEST_POT_OFF) ? "OK pot off\r\n" :
                   ((response->kind == ENV_ALARM_REQUEST_POT_TEMP) ? "OK pot temp\r\n" : "OK pot hum\r\n"));
    }
    if (response->kind == ENV_ALARM_REQUEST_SAVE)
    {
        if (response->result == ENV_ALARM_REQUEST_OK)
        {
            return SerialPort_WriteString("OK save; STORED\r\n");
        }
        if (response->result == ENV_ALARM_REQUEST_SAVE_UNCHANGED)
        {
            return SerialPort_WriteString("OK save; UNCHANGED\r\n");
        }
        return SerialPort_WriteString("ERR SAVE; RETRY\r\n");
    }
    if (response->kind == ENV_ALARM_REQUEST_SET_BOTH)
    {
        return send_combined_response(response);
    }
    if (response->kind == ENV_ALARM_REQUEST_SET_TEMPERATURE)
    {
        field = "temp";
        unit = "C";
        on_value = response->config.temperature_on_x10;
        off_value = response->config.temperature_off_x10;
        minimum = ENV_ALARM_TEMP_HYSTERESIS_X10;
        maximum = ENV_ALARM_TEMP_SET_MAX_X10;
    }
    else
    {
        field = "hum";
        unit = "%";
        on_value = response->config.humidity_on_x10;
        off_value = response->config.humidity_off_x10;
        minimum = ENV_ALARM_HUM_HYSTERESIS_X10;
        maximum = ENV_ALARM_HUM_SET_MAX_X10;
    }
    if (response->result == ENV_ALARM_REQUEST_OK)
    {
        length = snprintf(text, sizeof(text), "OK set %s on=%u.%u%s off=%u.%u%s\r\n",
                          field, (unsigned int)(on_value / 10U), (unsigned int)(on_value % 10U), unit,
                          (unsigned int)(off_value / 10U), (unsigned int)(off_value % 10U), unit);
    }
    else if (response->result == ENV_ALARM_REQUEST_RANGE_ERROR)
    {
        length = snprintf(text, sizeof(text), "ERR RANGE %s=%u.%u..%u.%u%s\r\n",
                          field, (unsigned int)(minimum / 10U), (unsigned int)(minimum % 10U),
                          (unsigned int)(maximum / 10U), (unsigned int)(maximum % 10U), unit);
    }
    else
    {
        return SerialPort_WriteString("ERR BAD_REQUEST\r\n");
    }
    if ((length < 0) || ((unsigned int)length >= sizeof(text)))
    {
        return HAL_ERROR;
    }
    return SerialPort_Write((const uint8_t *)text, (uint16_t)length);
}

void SerialCommand_UpdateSample(const EnvAlarm_Sample *sample, uint32_t now_ms)
{
    if (sample == NULL)
    {
        return;
    }
    s_serial_command.latest_sample = *sample;
    s_serial_command.last_sample_ms = now_ms;
    s_serial_command.has_sample = 1U;
}

HAL_StatusTypeDef SerialCommand_ProcessByte(uint8_t byte, uint32_t now_ms)
{
    s_serial_command.last_byte_ms = now_ms;
    if ((byte == '\r') || (byte == '\n'))
    {
        return finish_line(now_ms);
    }
    if (s_serial_command.discard_reason != SERIAL_COMMAND_DISCARD_NONE)
    {
        return HAL_OK;
    }
    /* ASCII commands plus tab for optional edge whitespace; reject NUL too. */
    if (((byte < 0x20U) || (byte > 0x7EU)) && (byte != '\t'))
    {
        s_serial_command.length = 0U;
        s_serial_command.discard_reason = SERIAL_COMMAND_DISCARD_INVALID_CHAR;
        return HAL_OK;
    }
    if (s_serial_command.length >= SERIAL_COMMAND_MAX_LENGTH)
    {
        s_serial_command.length = 0U;
        s_serial_command.discard_reason = SERIAL_COMMAND_DISCARD_TOO_LONG;
        return HAL_OK;
    }
    /* Normalize ASCII letters once; old uppercase commands stay usable. */
    if ((byte >= (uint8_t)'A') && (byte <= (uint8_t)'Z'))
    {
        byte = (uint8_t)(byte + ('a' - 'A'));
    }
    s_serial_command.line[s_serial_command.length] = (char)byte;
    ++s_serial_command.length;
    return HAL_OK;
}

HAL_StatusTypeDef SerialCommand_Poll(uint32_t now_ms)
{
    if ((s_serial_command.length == 0U) &&
        (s_serial_command.discard_reason == SERIAL_COMMAND_DISCARD_NONE))
    {
        return HAL_OK;
    }
    if ((uint32_t)(now_ms - s_serial_command.last_byte_ms) < SERIAL_COMMAND_IDLE_MS)
    {
        return HAL_OK;
    }
    /* Match only the complete buffered command, never an early get prefix. */
    return finish_line(now_ms);
}

void SerialCommand_DiscardLine(uint32_t now_ms)
{
    s_serial_command.length = 0U;
    s_serial_command.discard_reason = SERIAL_COMMAND_DISCARD_RX_LOSS;
    s_serial_command.last_byte_ms = now_ms;
}
