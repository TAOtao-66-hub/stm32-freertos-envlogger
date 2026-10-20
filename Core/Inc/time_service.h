#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H
#include <stdint.h>

typedef struct
{
    uint32_t timestamp;       /* Unix seconds, valid only when calibrated=1. */
    uint32_t uptime_seconds;
    uint8_t calibrated;
} TimeService_Snapshot;

/* Boot policy: current time is uncalibrated after every firmware restart.
 * Persisted historical anchors do not validate the current RTC after reset. */
uint8_t TimeService_Init(void);
/* Calendar text is local UTC+8 by default; exact YYYY MM DD HH:MM:SS. */
uint8_t TimeService_Parse(const char *text, uint32_t *timestamp);
void TimeService_Format(uint32_t timestamp, char text[20]);
uint8_t TimeService_SetText(const char *text);
/* Accept a parsed Unix timestamp in the supported Beijing calendar range. */
uint8_t TimeService_SetUnix(uint32_t timestamp);
void TimeService_Get(TimeService_Snapshot *snapshot);
#endif
