#ifndef SERIAL_COMMAND_H
#define SERIAL_COMMAND_H

#include <stdint.h>
#include "stm32f1xx_hal.h"
#include "env_alarm.h"
#include "FreeRTOS.h"
#include "queue.h"

/* All functions belong to serialTask; there is no ISR parser or shared state.
 * now_ms is HAL_GetTick(), passed in for explicit, testable timing. */
void SerialCommand_Init(void);
/* Bind a length-one EnvAlarm_Request queue after Init. Only one setting can
 * await application at a time; get/help continue to work while it is pending. */
void SerialCommand_BindAlarmQueue(QueueHandle_t request_queue);
/* serialTask receives the alarm owner's applied result and passes it here.
 * Success is never reported merely because a request entered a queue. */
HAL_StatusTypeDef SerialCommand_HandleAlarmResponse(const EnvAlarm_Response *response);
/* Copy the latest received result; failed acquisitions replace good ones too. */
void SerialCommand_UpdateSample(const EnvAlarm_Sample *sample, uint32_t now_ms);
/* Accept get/help/save, set temp <C>, set hum <%>, or both fields in one set.
 * Matching is case-insensitive. CR/LF can still complete a command.
 * Return HAL TX status, or HAL_OK when no response is required yet. */
HAL_StatusTypeDef SerialCommand_ProcessByte(uint8_t byte, uint32_t now_ms);
/* Call from serialTask after an empty RX wait. An idle gap completes the
 * buffered command, so the sender does not have to append CR/LF. */
HAL_StatusTypeDef SerialCommand_Poll(uint32_t now_ms);
/* RX loss: discard until a line ending or a complete idle gap. */
void SerialCommand_DiscardLine(uint32_t now_ms);

#endif /* SERIAL_COMMAND_H */
