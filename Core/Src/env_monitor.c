#include "env_monitor.h"
#include "oled.h"
#include "project_config.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* Private types ----------------------------------------------------------- */
typedef enum
{
    DISPLAY_ROW_TITLE = 0,
    DISPLAY_ROW_TEMPERATURE,
    DISPLAY_ROW_HUMIDITY,
    DISPLAY_ROW_THRESHOLDS,
    DISPLAY_ROW_ALARM,
    DISPLAY_ROW_SOUND,
    DISPLAY_ROW_SENSOR,
    DISPLAY_ROW_KEY_HINT
} EnvMonitor_DisplayRow;

typedef struct
{
    uint32_t last_sample_ms;
    uint8_t sensor_timing_ready;
    /* Only cache text that has been successfully sent to the OLED. */
    char displayed_rows[OLED_TEXT_ROWS][OLED_TEXT_COLUMNS + 1U];
    uint8_t displayed_row_valid[OLED_TEXT_ROWS];
} EnvMonitor_Context;

/* Module variables -------------------------------------------------------- */
/* Public debug snapshot; envMonitorTask is its only writer. */
volatile EnvMonitor_Status g_env_monitor;
/* Private working state; it is never exposed by env_monitor.h. */
static EnvMonitor_Context s_env_monitor;

/* Private functions ------------------------------------------------------- */

static uint8_t update_display_row(EnvMonitor_DisplayRow row, const char *text)
{
    if ((s_env_monitor.displayed_row_valid[row] != 0U) &&
        (strncmp(s_env_monitor.displayed_rows[row], text, OLED_TEXT_COLUMNS) == 0))
    {
        return 1U;
    }

    /* Clear old trailing characters before drawing a possibly shorter string.
     * Neither operation sends a blank intermediate frame to the display. */
    OLED_ClearRow(row);
    OLED_WriteString(row, 0U, text);
    if (OLED_UpdatePage(row) == 0U)
    {
        return 0U;
    }

    (void)strncpy(s_env_monitor.displayed_rows[row], text, OLED_TEXT_COLUMNS);
    s_env_monitor.displayed_rows[row][OLED_TEXT_COLUMNS] = '\0';
    s_env_monitor.displayed_row_valid[row] = 1U;
    return 1U;
}

static uint8_t draw_status(void)
{
    char text[OLED_TEXT_COLUMNS + 1U];
    uint16_t temperature = g_env_monitor.reading.temperature_x10;
    uint16_t humidity = g_env_monitor.reading.humidity_x10;
    EnvAlarm_State alarm;

    alarm = g_env_monitor.alarm;

    if (update_display_row(DISPLAY_ROW_TITLE, "DHT11 ALARM") == 0U)
    {
        return 0U;
    }
    if (g_env_monitor.reading_valid != 0U)
    {
        (void)snprintf(text, sizeof(text), "TEMP: %u.%u C",
                       (unsigned int)(temperature / 10U), (unsigned int)(temperature % 10U));
        if (update_display_row(DISPLAY_ROW_TEMPERATURE, text) == 0U)
        {
            return 0U;
        }
        (void)snprintf(text, sizeof(text), "HUM : %u.%u %%",
                       (unsigned int)(humidity / 10U), (unsigned int)(humidity % 10U));
        if (update_display_row(DISPLAY_ROW_HUMIDITY, text) == 0U)
        {
            return 0U;
        }
    }
    else
    {
        if ((update_display_row(DISPLAY_ROW_TEMPERATURE, "TEMP: --.- C") == 0U) ||
            (update_display_row(DISPLAY_ROW_HUMIDITY, "HUM : --.- %") == 0U))
        {
            return 0U;
        }
    }

    (void)snprintf(text, sizeof(text), "HI:%u.%uC / %u.%u%%",
                   (unsigned int)(alarm.config.temperature_on_x10 / 10U),
                   (unsigned int)(alarm.config.temperature_on_x10 % 10U),
                   (unsigned int)(alarm.config.humidity_on_x10 / 10U),
                   (unsigned int)(alarm.config.humidity_on_x10 % 10U));
    if (update_display_row(DISPLAY_ROW_THRESHOLDS, text) == 0U)
    {
        return 0U;
    }
    (void)snprintf(text, sizeof(text), "ALARM: %s", EnvAlarm_StatusText(&alarm));
    if (update_display_row(DISPLAY_ROW_ALARM, text) == 0U)
    {
        return 0U;
    }
    (void)snprintf(text, sizeof(text), "SOUND: %s",
                   (alarm.flags == 0U) ? "OFF" : ((alarm.muted != 0U) ? "MUTED" : "ON"));
    if (update_display_row(DISPLAY_ROW_SOUND, text) == 0U)
    {
        return 0U;
    }
    (void)snprintf(text, sizeof(text), "SENSOR: %s", DHT11_StatusText(g_env_monitor.sensor_status));
    if (update_display_row(DISPLAY_ROW_SENSOR, text) == 0U)
    {
        return 0U;
    }
    return update_display_row(DISPLAY_ROW_KEY_HINT,
                (alarm.control_mode == 0U) ? "KEY: PRESS TO MUTE" :
                ((alarm.control_mode == 1U) ? "CONTROL: POT TEMP" : "CONTROL: POT HUM"));
}

/* Public functions -------------------------------------------------------- */
void EnvMonitor_Init(void)
{
    EnvAlarm_State initial_alarm;
    s_env_monitor.sensor_timing_ready = DHT11_Init();
    g_env_monitor.reading.temperature_x10 = 0U;
    g_env_monitor.reading.humidity_x10 = 0U;
    g_env_monitor.reading_valid = 0U;
    EnvAlarm_Init(&initial_alarm);
    g_env_monitor.alarm = initial_alarm;
    g_env_monitor.sample_count = 0U;
    g_env_monitor.error_count = 0U;
    g_env_monitor.last_success_ms = 0U;
    g_env_monitor.sensor_status = (s_env_monitor.sensor_timing_ready != 0U) ? DHT11_WAIT : DHT11_TIMING_ERROR;
    /* Board power-up settling; this runs after the scheduler starts. */
    vTaskDelay(pdMS_TO_TICKS(ENV_MONITOR_POWER_UP_DELAY_MS));
    g_env_monitor.oled_ready = 0U;
    g_env_monitor.oled_address = 0U;
    s_env_monitor.last_sample_ms = HAL_GetTick();
    EnvMonitor_Refresh();
}

uint8_t EnvMonitor_Poll(void)
{
    uint32_t now = HAL_GetTick();
    DHT11_Reading reading;
    DHT11_Status status;
    if ((uint32_t)(now - s_env_monitor.last_sample_ms) < ENV_MONITOR_SAMPLE_INTERVAL_MS)
    {
        return 0U;
    }
    status = (s_env_monitor.sensor_timing_ready != 0U) ? DHT11_Read(&reading) : DHT11_TIMING_ERROR;
    if (status == DHT11_WAIT)
    {
        /* No acquisition happened. Preserve the last result and retry on a
         * later task poll; do not publish a false sensor-fault sample. */
        return 0U;
    }

    /* Schedule the next attempt from the end of this acquisition. */
    s_env_monitor.last_sample_ms = HAL_GetTick();
    g_env_monitor.sensor_status = status;
    if (status == DHT11_OK)
    {
        g_env_monitor.reading.temperature_x10 = reading.temperature_x10;
        g_env_monitor.reading.humidity_x10 = reading.humidity_x10;
        g_env_monitor.reading_valid = 1U;
        g_env_monitor.last_success_ms = HAL_GetTick();
        ++g_env_monitor.sample_count;
    }
    else
    {
        /* Keep the last good values for debugging, but hide them as stale on screen. */
        g_env_monitor.reading_valid = 0U;
        ++g_env_monitor.error_count;
    }

    return 1U;
}

void EnvMonitor_GetSample(EnvAlarm_Sample *sample)
{
    if (sample == NULL)
    {
        return;
    }

    /* Same-task copy: other tasks receive this value through the queue. */
    sample->temperature_x10 = g_env_monitor.reading.temperature_x10;
    sample->humidity_x10 = g_env_monitor.reading.humidity_x10;
    sample->valid = g_env_monitor.reading_valid;
}

void EnvMonitor_SetAlarmState(const EnvAlarm_State *state)
{
    /* Called only by envMonitorTask after receiving a copied queue message. */
    g_env_monitor.alarm = *state;
}

void EnvMonitor_Refresh(void)
{
    if (g_env_monitor.oled_ready == 0U)
    {
        g_env_monitor.oled_ready = OLED_Init();
        g_env_monitor.oled_address = OLED_GetAddress();
        /* Initialization clears OLED RAM, so cached text must not skip any row.
         * The next successful draw resends every row, including static labels. */
        (void)memset(s_env_monitor.displayed_row_valid, 0, sizeof(s_env_monitor.displayed_row_valid));
    }

    if (g_env_monitor.oled_ready != 0U)
    {
        g_env_monitor.oled_ready = draw_status();
    }
}
