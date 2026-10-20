#ifndef ENV_MONITOR_H
#define ENV_MONITOR_H

#include "dht11.h"
#include "env_alarm.h"

/* Debug snapshot: watch g_env_monitor in Keil even if OLED is disconnected.
 * Only envMonitorTask writes it; do not read it from another task without
 * adding a queue or a mutex-protected snapshot. Values persist only in SRAM. */
typedef struct
{
    DHT11_Reading reading;
    DHT11_Status sensor_status;
    EnvAlarm_State alarm;
    uint32_t sample_count;
    uint32_t error_count;
    uint32_t last_success_ms;
    uint8_t reading_valid;
    uint8_t oled_ready;
    uint8_t oled_address;
} EnvMonitor_Status;

extern volatile EnvMonitor_Status g_env_monitor;

void EnvMonitor_Init(void);
/* Return 1 for a completed result, 0 if not due or the driver reports WAIT. */
uint8_t EnvMonitor_Poll(void);
/* Copy the latest result; call from envMonitorTask, then send it by queue. */
void EnvMonitor_GetSample(EnvAlarm_Sample *sample);
void EnvMonitor_SetAlarmState(const EnvAlarm_State *state);
void EnvMonitor_Refresh(void);

#endif
