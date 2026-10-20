#include "project_commands.h"
#include "serial_port.h"
#include "potentiometer.h"
#include "time_service.h"
#include "rtos_diagnostics.h"
#include <stdio.h>
#include <string.h>

/* Private state belongs exclusively to serialTask. */
typedef struct
{
    QueueHandle_t requests;
    QueueHandle_t replies;
    uint32_t last_reply_ms;
    uint32_t request_id;
    uint32_t cancel_id;
    uint32_t clear_confirm_ms;
    uint8_t pending;
    uint8_t transfer;
    uint8_t cancel_pending;
    uint8_t deferred_notice;
    uint8_t clear_armed;
    uint8_t clearing;
} ProjectCommands_Context;

static ProjectCommands_Context s_commands;

void ProjectCommands_Bind(QueueHandle_t request, QueueHandle_t replies)
{
    (void)memset(&s_commands, 0, sizeof(s_commands));
    s_commands.requests = request;
    s_commands.replies = replies;
}

uint8_t ProjectCommands_TransferActive(void) { return s_commands.transfer; }
void ProjectCommands_DeferNotice(void) { s_commands.deferred_notice = 1U; }

static void request_cancel(void)
{
    s_commands.cancel_id = s_commands.request_id;
    s_commands.cancel_pending = 1U;
}

/* Decimal uint32 without signs, suffixes or arithmetic wraparound. */
static uint8_t parse_unsigned(const char **cursor, uint32_t *value)
{
    uint32_t digit;
    *value = 0U;
    if ((**cursor < '0') || (**cursor > '9')) { return 0U; }
    while ((**cursor >= '0') && (**cursor <= '9'))
    {
        digit = (uint32_t)(*(*cursor)++ - '0');
        if (*value > (0xFFFFFFFFUL - digit) / 10U) { return 0U; }
        *value = *value * 10U + digit;
    }
    return 1U;
}

HAL_StatusTypeDef ProjectCommands_Execute(const char *command, uint8_t *handled)
{
    Storage_Request request;
    TimeService_Snapshot time;
    Pot_Snapshot pot;
    char date[20];
    char text[112];
    const char *cursor;
    uint32_t count = 0U;

    (void)memset(&request, 0, sizeof(request));
    *handled = 1U;
    /* Reject other commands during a stream; report only after its end marker.
     * No command is silently saved for later application. */
    if (s_commands.transfer != 0U)
    {
        if ((strcmp(command, "log cancel") == 0) && (s_commands.clearing == 0U)) { request_cancel(); }
        else { ProjectCommands_DeferNotice(); }
        return HAL_OK;
    }
    if ((strcmp(command, "log clear") != 0) && (strcmp(command, "log clear confirm") != 0))
    { s_commands.clear_armed = 0U; }
    if (strcmp(command, "log cancel") == 0)
    { return SerialPort_WriteString("ERR NO_ACTIVE_TRANSFER\r\n"); }
    if (strcmp(command, "log clear") == 0)
    {
        if ((s_commands.requests == NULL) || (s_commands.pending != 0U) || (s_commands.cancel_pending != 0U))
        { return SerialPort_WriteString("ERR STORAGE_BUSY\r\n"); }
        s_commands.clear_armed = 1U;
        s_commands.clear_confirm_ms = HAL_GetTick();
        (void)snprintf(text, sizeof(text), "CONFIRM log clear confirm within %lums; HISTORY_ONLY\r\n",
                       (unsigned long)LOG_CLEAR_CONFIRM_WINDOW_MS);
        return SerialPort_WriteString(text);
    }
    if (strcmp(command, "diag") == 0) { return RtosDiagnostics_Print(); }
    if (strcmp(command, "help") == 0)
    {
        if (SerialPort_WriteString("commands: get | save | set temp <C> [hum <%>] | set hum <%>\r\n") != HAL_OK) { return HAL_ERROR; }
        if (SerialPort_WriteString("more: flash info | log status/read/page/export/cancel | time get/set | pot get/off/temp/hum\r\n") != HAL_OK) { return HAL_ERROR; }
        if (SerialPort_WriteString("history: log read 1..20 | log page <before:0=newest> <1..20> | log export\r\n") != HAL_OK) { return HAL_ERROR; }
        if (SerialPort_WriteString("maintenance: diag | log clear (then log clear confirm)\r\n") != HAL_OK) { return HAL_ERROR; }
        return SerialPort_WriteString("clock: time set YYYY MM DD HH:MM:SS\r\n");
    }
    if (strcmp(command, "time get") == 0)
    {
        TimeService_Get(&time);
        if (time.calibrated != 0U)
        {
            TimeService_Format(time.timestamp, date);
            (void)snprintf(text, sizeof(text), "TIME %s beijingtime uptime=%lu\r\n", date, (unsigned long)time.uptime_seconds);
        }
        else { (void)snprintf(text, sizeof(text), "TIME UNCALIBRATED uptime=%lu\r\n", (unsigned long)time.uptime_seconds); }
        return SerialPort_WriteString(text);
    }
    if (strcmp(command, "pot get") == 0)
    {
        Pot_Get(&pot);
        (void)snprintf(text, sizeof(text), "POT raw=%u filtered=%u mode=%s ready=%u\r\n",
                       (unsigned int)pot.raw, (unsigned int)pot.filtered,
                       (pot.mode == POT_MODE_OFF) ? "off" : ((pot.mode == POT_MODE_TEMPERATURE) ? "temp" : "hum"),
                       (unsigned int)pot.ready);
        return SerialPort_WriteString(text);
    }
    if (strcmp(command, "log clear confirm") == 0)
    {
        if ((s_commands.clear_armed == 0U) ||
            ((uint32_t)(HAL_GetTick() - s_commands.clear_confirm_ms) >= LOG_CLEAR_CONFIRM_WINDOW_MS))
        {
            s_commands.clear_armed = 0U;
            return SerialPort_WriteString("ERR CLEAR_CONFIRM; SEND log clear FIRST\r\n");
        }
        s_commands.clear_armed = 0U;
        request.kind = STORAGE_REQUEST_CLEAR;
    }
    else if (strncmp(command, "time set ", 9U) == 0)
    {
        if (TimeService_Parse(command + 9U, &request.timestamp) == 0U)
        { return SerialPort_WriteString("ERR TIME; USE time set YYYY MM DD HH:MM:SS (2020..2099)\r\n"); }
        request.kind = STORAGE_REQUEST_TIME_SET;
    }
    else if (strcmp(command, "flash info") == 0) { request.kind = STORAGE_REQUEST_INFO; }
    else if (strcmp(command, "log status") == 0) { request.kind = STORAGE_REQUEST_STATUS; }
    else if (strncmp(command, "log read ", 9U) == 0)
    {
        cursor = command + 9U;
        if ((parse_unsigned(&cursor, &count) == 0U) || (*cursor != '\0') ||
            (count == 0U) || (count > HISTORY_PAGE_MAX_RECORDS))
        { return SerialPort_WriteString("ERR LOG_COUNT; USE log read 1..20\r\n"); }
        request.kind = STORAGE_REQUEST_READ;
        request.count = (uint16_t)count;
    }
    else if (strncmp(command, "log page ", 9U) == 0)
    {
        cursor = command + 9U;
        if ((parse_unsigned(&cursor, &request.before_sequence) == 0U) || (*cursor != ' '))
        { return SerialPort_WriteString("ERR LOG_PAGE; USE log page <before:0=newest> <1..20>\r\n"); }
        ++cursor;
        if ((parse_unsigned(&cursor, &count) == 0U) || (*cursor != '\0') ||
            (count == 0U) || (count > HISTORY_PAGE_MAX_RECORDS))
        { return SerialPort_WriteString("ERR LOG_PAGE; USE log page <before:0=newest> <1..20>\r\n"); }
        request.kind = STORAGE_REQUEST_PAGE;
        request.count = (uint16_t)count;
    }
    else if (strcmp(command, "log export") == 0) { request.kind = STORAGE_REQUEST_EXPORT; }
    else { *handled = 0U; return HAL_OK; }
    if ((s_commands.requests == NULL) || (s_commands.pending != 0U) || (s_commands.cancel_pending != 0U))
    { return SerialPort_WriteString("ERR STORAGE_BUSY\r\n"); }
    ++s_commands.request_id;
    if (s_commands.request_id == 0U) { ++s_commands.request_id; }
    request.request_id = s_commands.request_id;
    if (xQueueSend(s_commands.requests, &request, 0U) != pdPASS) { return SerialPort_WriteString("ERR STORAGE_BUSY\r\n"); }
    s_commands.pending = 1U;
    s_commands.clearing = (request.kind == STORAGE_REQUEST_CLEAR) ? 1U : 0U;
    s_commands.transfer = ((request.kind == STORAGE_REQUEST_PAGE) || (request.kind == STORAGE_REQUEST_EXPORT) ||
                           (s_commands.clearing != 0U)) ? 1U : 0U;
    s_commands.last_reply_ms = HAL_GetTick();
    return HAL_OK;
}

void ProjectCommands_Service(void)
{
    Storage_TextReply reply;
    Storage_Request cancel;
    if ((s_commands.cancel_pending != 0U) && (s_commands.requests != NULL))
    {
        (void)memset(&cancel, 0, sizeof(cancel));
        cancel.kind = STORAGE_REQUEST_CANCEL;
        cancel.request_id = s_commands.cancel_id;
        if (xQueueSend(s_commands.requests, &cancel, 0U) == pdPASS) { s_commands.cancel_pending = 0U; }
    }
    if ((s_commands.replies != NULL) && (xQueueReceive(s_commands.replies, &reply, 0U) == pdPASS))
    {
        if ((s_commands.pending != 0U) && (reply.request_id == s_commands.request_id))
        {
            s_commands.last_reply_ms = HAL_GetTick();
            if ((reply.final != 2U) && (SerialPort_WriteString(reply.text) != HAL_OK))
            {
                if ((s_commands.transfer != 0U) && (s_commands.clearing == 0U)) { request_cancel(); }
                s_commands.pending = s_commands.transfer = s_commands.clearing = 0U;
                (void)SerialPort_WriteString("ERR STORAGE_TX; TRANSFER_INCOMPLETE\r\n");
            }
            else if (reply.final == 1U) { s_commands.pending = s_commands.transfer = s_commands.clearing = 0U; }
        }
    }
    /* Idle timeout: each matching data/progress reply renews the deadline. */
    if ((s_commands.pending != 0U) && ((uint32_t)(HAL_GetTick() - s_commands.last_reply_ms) >= STORAGE_COMMAND_TIMEOUT_MS))
    {
        if ((s_commands.transfer != 0U) && (s_commands.clearing == 0U)) { request_cancel(); }
        s_commands.pending = s_commands.transfer = s_commands.clearing = 0U;
        (void)SerialPort_WriteString("ERR STORAGE_TIMEOUT\r\n");
    }
    if ((s_commands.transfer == 0U) && (s_commands.deferred_notice != 0U))
    {
        s_commands.deferred_notice = 0U;
        (void)SerialPort_WriteString("ERR TRANSFER_BUSY; OTHER_COMMAND_OR_RX_ERROR_IGNORED; RESEND\r\n");
    }
}
