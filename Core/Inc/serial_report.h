#ifndef SERIAL_REPORT_H
#define SERIAL_REPORT_H

#include "stm32f1xx_hal.h"
#include "env_alarm.h"

/* Task-only: format and send one copied acquisition result.
 * Call only from serialTask, which owns all USART1 writes.
 * Invalid samples use placeholders, never the retained last-good numbers.
 * Return HAL status; there is no retry or persistent state in this module. */
HAL_StatusTypeDef SerialReport_SendSample(const EnvAlarm_Sample *sample);
/* Same payload with a get prefix, so query replies differ from ENV reports. */
HAL_StatusTypeDef SerialReport_SendQuery(const EnvAlarm_Sample *sample);

#endif /* SERIAL_REPORT_H */
