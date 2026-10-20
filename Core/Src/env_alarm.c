#include "env_alarm.h"

void EnvAlarm_Init(EnvAlarm_State *state)
{
    state->flags = 0U;
    state->muted = 0U;
    state->control_mode = 0U;
    state->config.temperature_on_x10 = ENV_ALARM_TEMP_ON_X10;
    state->config.temperature_off_x10 = ENV_ALARM_TEMP_OFF_X10;
    state->config.humidity_on_x10 = ENV_ALARM_HUM_ON_X10;
    state->config.humidity_off_x10 = ENV_ALARM_HUM_OFF_X10;
}

void EnvAlarm_Update(EnvAlarm_State *state, const EnvAlarm_Sample *sample)
{
    uint8_t previous_flags = state->flags;

    if (sample->valid == 0U)
    {
        /* Do not clear existing threshold alarms using an invalid reading. */
        state->flags |= ENV_ALARM_FLAG_SENSOR;
    }
    else
    {
        state->flags &= (uint8_t)~ENV_ALARM_FLAG_SENSOR;

        if (sample->temperature_x10 >= state->config.temperature_on_x10)
        {
            state->flags |= ENV_ALARM_FLAG_TEMPERATURE;
        }
        else if (sample->temperature_x10 <= state->config.temperature_off_x10)
        {
            state->flags &= (uint8_t)~ENV_ALARM_FLAG_TEMPERATURE;
        }

        if (sample->humidity_x10 >= state->config.humidity_on_x10)
        {
            state->flags |= ENV_ALARM_FLAG_HUMIDITY;
        }
        else if (sample->humidity_x10 <= state->config.humidity_off_x10)
        {
            state->flags &= (uint8_t)~ENV_ALARM_FLAG_HUMIDITY;
        }
    }

    /* Recovery clears mute. A newly appearing cause starts sound again. */
    if ((state->flags == 0U) ||
        ((state->flags & (uint8_t)~previous_flags) != 0U))
    {
        state->muted = 0U;
    }
}

void EnvAlarm_Mute(EnvAlarm_State *state)
{
    if (state->flags != 0U)
    {
        state->muted = 1U;
    }
}

void EnvAlarm_ApplyRequest(EnvAlarm_State *state, const EnvAlarm_Request *request,
                           const EnvAlarm_Sample *sample, EnvAlarm_Response *response)
{
    EnvAlarm_Config next_config = state->config;
    uint8_t set_temperature = ((request->kind == ENV_ALARM_REQUEST_SET_TEMPERATURE) ||
                               (request->kind == ENV_ALARM_REQUEST_SET_BOTH)) ? 1U : 0U;
    uint8_t set_humidity = ((request->kind == ENV_ALARM_REQUEST_SET_HUMIDITY) ||
                            (request->kind == ENV_ALARM_REQUEST_SET_BOTH)) ? 1U : 0U;

    response->request_id = request->request_id;
    response->kind = request->kind;
    response->result = ENV_ALARM_REQUEST_INVALID;

    if ((set_temperature == 0U) && (set_humidity == 0U))
    {
        /* Unknown requests leave both configuration and alarm state intact. */
    }
    else if (((set_temperature != 0U) &&
              ((request->temperature_x10 < ENV_ALARM_TEMP_HYSTERESIS_X10) ||
               (request->temperature_x10 > ENV_ALARM_TEMP_SET_MAX_X10))) ||
             ((set_humidity != 0U) &&
              ((request->humidity_x10 < ENV_ALARM_HUM_HYSTERESIS_X10) ||
               (request->humidity_x10 > ENV_ALARM_HUM_SET_MAX_X10))))
    {
        response->result = ENV_ALARM_REQUEST_RANGE_ERROR;
    }
    else
    {
        if (set_temperature != 0U)
        {
            next_config.temperature_on_x10 = request->temperature_x10;
            next_config.temperature_off_x10 = request->temperature_x10 - ENV_ALARM_TEMP_HYSTERESIS_X10;
        }
        if (set_humidity != 0U)
        {
            next_config.humidity_on_x10 = request->humidity_x10;
            next_config.humidity_off_x10 = request->humidity_x10 - ENV_ALARM_HUM_HYSTERESIS_X10;
        }

        /* alarmTask is the sole owner. Commit only after every field passed;
         * never evaluate/publish an intermediate temperature-only setting. */
        state->config = next_config;
        response->result = ENV_ALARM_REQUEST_OK;
        if (sample != 0)
        {
            EnvAlarm_Update(state, sample);
        }
    }
    response->config = state->config;
}

const char *EnvAlarm_StatusText(const EnvAlarm_State *state)
{
    if ((state->flags & ENV_ALARM_FLAG_SENSOR) != 0U)
    {
        return "SENSOR FAULT";
    }

    switch (state->flags)
    {
    case ENV_ALARM_FLAG_TEMPERATURE:
        return "TEMP HIGH";
    case ENV_ALARM_FLAG_HUMIDITY:
        return "HUM HIGH";
    case ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY:
        return "TEMP+HUM HIGH";
    default:
        return "NORMAL";
    }
}
