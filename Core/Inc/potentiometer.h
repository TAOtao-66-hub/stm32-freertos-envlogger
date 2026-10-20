#ifndef POTENTIOMETER_H
#define POTENTIOMETER_H
#include "env_alarm.h"

typedef enum { POT_MODE_OFF = 0, POT_MODE_TEMPERATURE, POT_MODE_HUMIDITY } Pot_Mode;
typedef struct
{
    uint16_t raw;
    uint16_t filtered;
    Pot_Mode mode;
    uint8_t ready;
} Pot_Snapshot;

uint8_t Pot_Init(void);
/* Mode and Poll belong to alarmTask; serialTask only reads snapshots. */
uint8_t Pot_SetMode(Pot_Mode mode);
uint8_t Pot_Poll(uint32_t now_ms, EnvAlarm_Request *request);
void Pot_Get(Pot_Snapshot *snapshot);
#endif
