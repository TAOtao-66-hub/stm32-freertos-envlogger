#include "oled.h"
#include "soft_i2c.h"
#include "env_monitor.h"
#include "task.h"
#include <string.h>

/* Run the real OLED/application code against an I2C display model.
 * These tests never access the driver's private framebuffer/cache. */
static uint8_t screen[OLED_PAGE_COUNT][OLED_WIDTH];
static uint8_t previous_screen[OLED_PAGE_COUNT][OLED_WIDTH];
static uint8_t selected_page;
static uint8_t page_mask;
static uint8_t fail_command;
static uint8_t fail_data;
static uint8_t device_present;
static uint32_t write_calls;
static uint32_t data_bytes;
static uint32_t protocol_errors;
static uint32_t failures;
static uint32_t test_tick;
static uint32_t sensor_read_calls;
static DHT11_Status next_sensor_status;
static DHT11_Reading next_reading;

#define CHECK(group, condition) \
    do { if (!(condition)) { failures |= (1UL << (group)); } } while (0)

uint8_t SoftI2C_Init(void) { return 1U; }
uint8_t SoftI2C_Probe(uint8_t address)
{
    return (uint8_t)((device_present != 0U) && (address == 0x3CU));
}

uint8_t SoftI2C_Write(uint8_t address, uint8_t control,
                      const uint8_t *data, uint16_t length)
{
    ++write_calls;
    if ((address != 0x3CU) || (device_present == 0U)) { return 0U; }
    if ((control == 0x00U) && (length == 3U))
    {
        if ((data[0] < 0xB0U) || (data[0] >= 0xB8U) ||
            (data[1] != 0U) || (data[2] != 0x10U))
        {
            ++protocol_errors;
            return 0U;
        }
        selected_page = (uint8_t)(data[0] - 0xB0U);
        if (fail_command != 0U) { fail_command = 0U; return 0U; }
    }
    else if (control == 0x40U)
    {
        if (length != OLED_WIDTH) { ++protocol_errors; return 0U; }
        if (fail_data != 0U) { fail_data = 0U; return 0U; }
        (void)memcpy(screen[selected_page], data, length);
        page_mask |= (uint8_t)(1U << selected_page);
        data_bytes += length;
    }
    return 1U;
}

uint32_t HAL_GetTick(void) { return test_tick; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
uint8_t DHT11_Init(void) { return 1U; }
DHT11_Status DHT11_Read(DHT11_Reading *reading)
{
    ++sensor_read_calls;
    if (next_sensor_status == DHT11_OK)
    {
        *reading = next_reading;
    }
    return next_sensor_status;
}
const char *DHT11_StatusText(DHT11_Status status)
{
    if (status == DHT11_OK) { return "OK"; }
    if (status == DHT11_TIMEOUT) { return "TIMEOUT"; }
    return "WAIT";
}

static void reset_transfer_counts(void)
{
    write_calls = 0U;
    data_bytes = 0U;
    page_mask = 0U;
}

static uint8_t other_pages_unchanged(uint8_t changed_page)
{
    uint8_t page;
    for (page = 0U; page < OLED_PAGE_COUNT; ++page)
    {
        if ((page != changed_page) &&
            (memcmp(screen[page], previous_screen[page], OLED_WIDTH) != 0))
        {
            return 0U;
        }
    }
    return 1U;
}

uint32_t RunOledRefreshTests(void)
{
    uint16_t column;
    uint8_t page;
    uint8_t nonzero;
    EnvAlarm_Sample sample;
    EnvAlarm_State state;

    device_present = 1U;
    CHECK(0, OLED_UpdatePage(0U) == 0U);
    CHECK(0, OLED_UpdatePage(OLED_PAGE_COUNT) == 0U);
    CHECK(0, write_calls == 0U);

    EnvMonitor_Init();
    CHECK(1, g_env_monitor.oled_ready == 1U);
    CHECK(1, (data_bytes == 2048U) && (page_mask == 0xFFU));
    for (page = 0U; page < OLED_PAGE_COUNT; ++page)
    {
        nonzero = 0U;
        for (column = 0U; column < OLED_WIDTH; ++column)
        {
            nonzero |= screen[page][column];
        }
        CHECK(1, nonzero != 0U);
    }
    reset_transfer_counts();
    EnvMonitor_Refresh();
    CHECK(1, write_calls == 0U);

    g_env_monitor.reading.temperature_x10 = 250U;
    g_env_monitor.reading.humidity_x10 = 500U;
    g_env_monitor.reading_valid = 1U;
    g_env_monitor.sensor_status = DHT11_OK;
    EnvMonitor_Refresh();
    CHECK(2, (data_bytes == 384U) && (page_mask == 0x46U));
    (void)memcpy(previous_screen, screen, sizeof(screen));
    reset_transfer_counts();
    g_env_monitor.reading.temperature_x10 = 260U;
    EnvMonitor_Refresh();
    CHECK(2, (data_bytes == 128U) && (page_mask == 0x02U));
    CHECK(2, other_pages_unchanged(1U) != 0U);
    reset_transfer_counts();
    g_env_monitor.reading.humidity_x10 = 510U;
    EnvMonitor_Refresh();
    CHECK(2, (data_bytes == 128U) && (page_mask == 0x04U));
    reset_transfer_counts();
    EnvMonitor_Refresh();
    CHECK(2, write_calls == 0U);

    g_env_monitor.alarm.flags = ENV_ALARM_FLAG_TEMPERATURE;
    EnvMonitor_Refresh();
    CHECK(3, (data_bytes == 256U) && (page_mask == 0x30U));
    reset_transfer_counts();
    g_env_monitor.alarm.muted = 1U;
    EnvMonitor_Refresh();
    CHECK(3, (data_bytes == 128U) && (page_mask == 0x20U));
    reset_transfer_counts();
    g_env_monitor.alarm.muted = 0U;
    EnvMonitor_Refresh();
    CHECK(3, (data_bytes == 128U) && (page_mask == 0x20U));
    /* "SOUND: MUTED" -> "SOUND: ON": the old suffix must disappear. */
    for (column = 9U * 6U; column < OLED_WIDTH; ++column)
    {
        CHECK(3, screen[5U][column] == 0U);
    }

    reset_transfer_counts();
    g_env_monitor.reading_valid = 0U;
    g_env_monitor.sensor_status = DHT11_TIMEOUT;
    EnvMonitor_Refresh();
    CHECK(4, (data_bytes == 384U) && (page_mask == 0x46U));
    reset_transfer_counts();
    g_env_monitor.reading_valid = 1U;
    g_env_monitor.sensor_status = DHT11_OK;
    EnvMonitor_Refresh();
    CHECK(4, (data_bytes == 384U) && (page_mask == 0x46U));

    reset_transfer_counts();
    g_env_monitor.reading.temperature_x10 = 270U;
    fail_command = 1U;
    EnvMonitor_Refresh();
    CHECK(5, (g_env_monitor.oled_ready == 0U) && (data_bytes == 0U));
    reset_transfer_counts();
    EnvMonitor_Refresh();
    CHECK(5, (g_env_monitor.oled_ready == 1U) && (data_bytes == 2048U));
    reset_transfer_counts();
    EnvMonitor_Refresh();
    CHECK(5, write_calls == 0U);

    g_env_monitor.reading.humidity_x10 = 520U;
    fail_data = 1U;
    EnvMonitor_Refresh();
    CHECK(5, g_env_monitor.oled_ready == 0U);
    device_present = 0U;
    reset_transfer_counts();
    EnvMonitor_Refresh();
    CHECK(5, (g_env_monitor.oled_ready == 0U) && (write_calls == 0U));
    device_present = 1U;
    EnvMonitor_Refresh();
    CHECK(5, (g_env_monitor.oled_ready == 1U) && (data_bytes == 2048U));
    reset_transfer_counts();
    EnvMonitor_Refresh();
    CHECK(5, write_calls == 0U);

    /* Invalid clear/update requests must leave valid display data intact. */
    (void)memcpy(previous_screen, screen, sizeof(screen));
    OLED_ClearRow(OLED_TEXT_ROWS);
    CHECK(0, OLED_UpdatePage(OLED_PAGE_COUNT) == 0U);
    CHECK(0, write_calls == 0U);
    CHECK(0, OLED_Update() == 1U);
    CHECK(0, memcmp(screen, previous_screen, sizeof(screen)) == 0);
    CHECK(0, protocol_errors == 0U);

    /* Application polling follows the configured interval, including rollover. */
    test_tick = 0U;
    next_sensor_status = DHT11_OK;
    next_reading.temperature_x10 = 250U;
    next_reading.humidity_x10 = 500U;
    sensor_read_calls = 0U;
    EnvMonitor_Init();
    test_tick = ENV_MONITOR_SAMPLE_INTERVAL_MS - 1U;
    CHECK(6, EnvMonitor_Poll() == 0U);
    CHECK(6, sensor_read_calls == 0U);
    ++test_tick;
    CHECK(6, EnvMonitor_Poll() == 1U);
    CHECK(6, sensor_read_calls == 1U);
    EnvMonitor_GetSample(&sample);
    CHECK(6, (sample.valid == 1U) && (sample.temperature_x10 == 250U) &&
             (sample.humidity_x10 == 500U));
    sample.temperature_x10 = 999U;
    CHECK(6, g_env_monitor.reading.temperature_x10 == 250U);
    EnvMonitor_GetSample(NULL);
    CHECK(6, EnvMonitor_Poll() == 0U);

    /* WAIT does not invalidate a good result, increment errors or publish data. */
    test_tick += ENV_MONITOR_SAMPLE_INTERVAL_MS;
    next_sensor_status = DHT11_WAIT;
    CHECK(7, EnvMonitor_Poll() == 0U);
    CHECK(7, (g_env_monitor.reading_valid == 1U) &&
             (g_env_monitor.sensor_status == DHT11_OK) &&
             (g_env_monitor.sample_count == 1U) && (g_env_monitor.error_count == 0U));
    reset_transfer_counts();
    EnvMonitor_Refresh();
    reset_transfer_counts();
    ++test_tick;
    CHECK(7, EnvMonitor_Poll() == 0U);
    EnvMonitor_Refresh();
    CHECK(7, write_calls == 0U);
    next_sensor_status = DHT11_TIMEOUT;
    ++test_tick;
    CHECK(7, EnvMonitor_Poll() == 1U);
    EnvMonitor_GetSample(&sample);
    CHECK(7, (sample.valid == 0U) && (g_env_monitor.error_count == 1U));

    test_tick = 0xFFFFFF00U;
    EnvMonitor_Init();
    next_sensor_status = DHT11_OK;
    test_tick += ENV_MONITOR_SAMPLE_INTERVAL_MS - 1U;
    CHECK(6, EnvMonitor_Poll() == 0U);
    ++test_tick;
    CHECK(6, EnvMonitor_Poll() == 1U);
    /* A config-only snapshot redraws the threshold row and owns its copy. */
    EnvMonitor_Refresh();
    (void)memcpy(previous_screen, screen, sizeof(screen));
    reset_transfer_counts();
    state = g_env_monitor.alarm;
    state.config.temperature_on_x10 = 350U;
    state.config.temperature_off_x10 = 330U;
    state.config.humidity_on_x10 = 800U;
    state.config.humidity_off_x10 = 750U;
    EnvMonitor_SetAlarmState(&state);
    state.config.temperature_on_x10 = 999U;
    EnvMonitor_Refresh();
    CHECK(8, g_env_monitor.alarm.config.temperature_on_x10 == 350U);
    CHECK(8, (data_bytes == 128U) && (page_mask == 0x08U));
    CHECK(8, other_pages_unchanged(3U) != 0U);
    CHECK(8, memcmp(screen[3U], previous_screen[3U], OLED_WIDTH) != 0);
    reset_transfer_counts();
    EnvMonitor_Refresh();
    CHECK(8, write_calls == 0U);
    return failures;
}
