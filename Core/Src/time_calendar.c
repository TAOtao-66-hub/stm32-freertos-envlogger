#include "time_service.h"
#include "project_config.h"
#include <stdio.h>
#include <string.h>

static uint16_t month_days(uint16_t year, uint16_t month)
{
    static const uint8_t days[12] = {31U,28U,31U,30U,31U,30U,31U,31U,30U,31U,30U,31U};
    return (uint16_t)(days[month - 1U] + (((month == 2U) && ((year % 4U) == 0U) &&
             (((year % 100U) != 0U) || ((year % 400U) == 0U))) ? 1U : 0U));
}

uint8_t TimeService_Parse(const char *text, uint32_t *timestamp)
{
    uint16_t value[6] = {0U};
    uint16_t year;
    uint16_t month;
    uint32_t days = 0U;
    uint8_t index;
    uint8_t field = 0U;

    if ((text == NULL) || (timestamp == NULL) || (strlen(text) != 19U)) { return 0U; }
    for (index = 0U; index < 19U; ++index)
    {
        if ((index == 4U) || (index == 7U) || (index == 10U) || (index == 13U) || (index == 16U))
        {
            if (text[index] != ((index <= 10U) ? ' ' : ':')) { return 0U; }
            ++field;
        }
        else
        {
            if ((text[index] < '0') || (text[index] > '9')) { return 0U; }
            value[field] = (uint16_t)(value[field] * 10U + (uint16_t)(text[index] - '0'));
        }
    }
    if ((value[0] < 2020U) || (value[0] > 2099U) || (value[1] < 1U) || (value[1] > 12U) ||
        (value[2] < 1U) || (value[2] > month_days(value[0], value[1])) ||
        (value[3] > 23U) || (value[4] > 59U) || (value[5] > 59U)) { return 0U; }
    for (year = 1970U; year < value[0]; ++year) { days += ((year % 4U) == 0U) ? 366U : 365U; }
    for (month = 1U; month < value[1]; ++month) { days += month_days(value[0], month); }
    days += value[2] - 1U;
    *timestamp = days * 86400UL + value[3] * 3600UL + value[4] * 60UL + value[5] - TIME_LOCAL_UTC_OFFSET_SECONDS;
    return 1U;
}

void TimeService_Format(uint32_t timestamp, char text[20])
{
    uint32_t local = timestamp + TIME_LOCAL_UTC_OFFSET_SECONDS;
    uint32_t days = local / 86400UL;
    uint32_t seconds = local % 86400UL;
    uint16_t year = 1970U;
    uint16_t month = 1U;
    uint16_t count;

    for (;;)
    {
        count = ((year % 4U) == 0U) ? 366U : 365U;
        if (days < count) { break; }
        days -= count;
        ++year;
    }
    while (days >= month_days(year, month)) { days -= month_days(year, month); ++month; }
    (void)snprintf(text, 20U, "%04u %02u %02u %02u:%02u:%02u", (unsigned int)year,
                   (unsigned int)month, (unsigned int)(days + 1U),
                   (unsigned int)(seconds / 3600U), (unsigned int)((seconds / 60U) % 60U),
                   (unsigned int)(seconds % 60U));
}

