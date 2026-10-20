#include "serial_report.h"
#include "serial_port.h"
#include <stdio.h>

/* Fixed protocol storage, including the terminating zero; not a timing setting.
 * This also fits the widest uint16_t x10 readings without floating point. */
#define SERIAL_REPORT_BUFFER_BYTES  64U

static HAL_StatusTypeDef send_sample(const char *prefix, const EnvAlarm_Sample *sample)
{
    char text[SERIAL_REPORT_BUFFER_BYTES];
    int length;

    if (sample == NULL)
    {
        return HAL_ERROR;
    }
    if (sample->valid == 0U)
    {
        length = snprintf(text, sizeof(text), "%s TEMP=--.-C HUM=--.-%% SENSOR=ERROR\r\n", prefix);
    }

    else
    {
        /* Readings are tenths: 256 means 25.6. No float printf needed. */
        length = snprintf(text, sizeof(text), "%s TEMP=%u.%uC HUM=%u.%u%% SENSOR=OK\r\n",
                          prefix,
                          (unsigned int)(sample->temperature_x10 / 10U),
                          (unsigned int)(sample->temperature_x10 % 10U),
                          (unsigned int)(sample->humidity_x10 / 10U),
                          (unsigned int)(sample->humidity_x10 % 10U));
    }
    if ((length < 0) || ((unsigned int)length >= sizeof(text)))
    {
        return HAL_ERROR;
    }
    return SerialPort_Write((const uint8_t *)text, (uint16_t)length);
}

HAL_StatusTypeDef SerialReport_SendSample(const EnvAlarm_Sample *sample)
{
    return send_sample("ENV", sample);
}

HAL_StatusTypeDef SerialReport_SendQuery(const EnvAlarm_Sample *sample)
{
    return send_sample("get", sample);
}
