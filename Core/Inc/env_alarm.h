#ifndef ENV_ALARM_H
#define ENV_ALARM_H

#include <stdint.h>
#include "project_config.h"

/* Public flag meanings, not tunable parameters. Settings: project_config.h. */
#define ENV_ALARM_FLAG_TEMPERATURE  0x01U
#define ENV_ALARM_FLAG_HUMIDITY     0x02U
#define ENV_ALARM_FLAG_SENSOR       0x04U

typedef struct
{
    uint16_t temperature_x10;
    uint16_t humidity_x10;
    uint8_t valid;
} EnvAlarm_Sample;

typedef struct
{
    uint16_t temperature_on_x10;
    uint16_t temperature_off_x10;
    uint16_t humidity_on_x10;
    uint16_t humidity_off_x10;
} EnvAlarm_Config;

typedef struct
{
    uint8_t flags;
    uint8_t muted;
    EnvAlarm_Config config;
    uint8_t control_mode; /* 0=remote, 1=pot temperature, 2=pot humidity. */
} EnvAlarm_State;

typedef enum
{
    ENV_ALARM_REQUEST_SET_TEMPERATURE = 0,
    ENV_ALARM_REQUEST_SET_HUMIDITY,
    ENV_ALARM_REQUEST_SET_BOTH,
    ENV_ALARM_REQUEST_SAVE,
    ENV_ALARM_REQUEST_POT_OFF,
    ENV_ALARM_REQUEST_POT_TEMP,
    ENV_ALARM_REQUEST_POT_HUM
} EnvAlarm_RequestKind;

typedef enum
{
    ENV_ALARM_REQUEST_OK = 0,
    ENV_ALARM_REQUEST_RANGE_ERROR,
    ENV_ALARM_REQUEST_INVALID,
    ENV_ALARM_REQUEST_SAVE_UNCHANGED,
    ENV_ALARM_REQUEST_STORAGE_ERROR,
    ENV_ALARM_REQUEST_LOCAL_CONTROL
} EnvAlarm_RequestResult;

/* The serial task submits a copied request; only alarmTask applies it. */
typedef struct
{
    uint32_t request_id;
    EnvAlarm_RequestKind kind;
    uint16_t temperature_x10;
    uint16_t humidity_x10;
} EnvAlarm_Request;

typedef struct
{
    uint32_t request_id;
    EnvAlarm_RequestKind kind;
    EnvAlarm_RequestResult result;
    EnvAlarm_Config config;
} EnvAlarm_Response;

/* Pure alarm rules. State belongs to alarmTask; queues carry value copies. */
void EnvAlarm_Init(EnvAlarm_State *state);
void EnvAlarm_Update(EnvAlarm_State *state, const EnvAlarm_Sample *sample);
void EnvAlarm_Mute(EnvAlarm_State *state);
/* Task-only owner operation: validate the complete request, then commit
 * selected fields together and reevaluate once. A combined request is all-or-nothing.
 * sample may be NULL before the first acquisition. Invalid requests change
 * neither configuration nor alarm state. The response carries applied values. */
void EnvAlarm_ApplyRequest(EnvAlarm_State *state, const EnvAlarm_Request *request,
                           const EnvAlarm_Sample *sample, EnvAlarm_Response *response);
const char *EnvAlarm_StatusText(const EnvAlarm_State *state);

#endif /* ENV_ALARM_H */
