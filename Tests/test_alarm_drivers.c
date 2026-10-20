#include "env_alarm.h"
#include "button.h"
#include "buzzer.h"
#include "led.h"
#include "gpio.h"

#define CHECK(condition) do { if (!(condition)) { return 1U; } } while (0)

GPIO_TypeDef test_gpio_a;
static uint32_t test_ms;
static GPIO_PinState test_button_level;
static GPIO_PinState test_led_level;
static GPIO_PinState test_buzzer_level;
static uint8_t wrong_gpio_access;

uint32_t HAL_GetTick(void)
{
    return test_ms;
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    if ((port != BUTTON_GPIO_Port) || (pin != BUTTON_Pin))
    {
        wrong_gpio_access = 1U;
    }
    return test_button_level;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level)
{
    if (port != &test_gpio_a)
    {
        wrong_gpio_access = 1U;
    }
    if (pin == LED_Pin)
    {
        test_led_level = level;
    }
    else if (pin == BUZZER_Pin)
    {
        test_buzzer_level = level;
    }
    else
    {
        wrong_gpio_access = 1U;
    }
}

static Button_Event button_step(uint32_t ms, GPIO_PinState level)
{
    test_ms = ms;
    test_button_level = level;
    return Button_Poll();
}

static void reading(EnvAlarm_State *state, uint16_t temperature, uint16_t humidity,
                    uint8_t valid)
{
    EnvAlarm_Sample sample;
    sample.temperature_x10 = temperature;
    sample.humidity_x10 = humidity;
    sample.valid = valid;
    EnvAlarm_Update(state, &sample);
}

static uint32_t test_hysteresis(void)
{
    EnvAlarm_State state;
    EnvAlarm_Init(&state);
    reading(&state, 299U, 699U, 1U);
    CHECK(state.flags == 0U);
    reading(&state, 300U, 699U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_TEMPERATURE);
    reading(&state, 299U, 699U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_TEMPERATURE);
    reading(&state, 281U, 699U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_TEMPERATURE);
    reading(&state, 280U, 699U, 1U);
    CHECK(state.flags == 0U);
    reading(&state, 280U, 700U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_HUMIDITY);
    reading(&state, 280U, 699U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_HUMIDITY);
    reading(&state, 280U, 651U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_HUMIDITY);
    reading(&state, 280U, 650U, 1U);
    CHECK(state.flags == 0U);
    reading(&state, 300U, 700U, 1U);
    CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY));
    reading(&state, 280U, 680U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_HUMIDITY);
    return 0U;
}

static uint32_t test_mute_recovery(void)
{
    EnvAlarm_State state;
    EnvAlarm_Init(&state);
    EnvAlarm_Mute(&state);
    CHECK(state.muted == 0U);
    reading(&state, 300U, 500U, 1U);
    EnvAlarm_Mute(&state);
    CHECK(state.muted == 1U);
    reading(&state, 350U, 500U, 1U);
    CHECK(state.muted == 1U);
    reading(&state, 290U, 500U, 1U);
    CHECK(state.muted == 1U && state.flags == ENV_ALARM_FLAG_TEMPERATURE);
    reading(&state, 280U, 500U, 1U);
    CHECK(state.flags == 0U && state.muted == 0U);
    reading(&state, 300U, 500U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_TEMPERATURE && state.muted == 0U);
    return 0U;
}

static uint32_t test_new_cause(void)
{
    EnvAlarm_State state;
    EnvAlarm_Init(&state);
    reading(&state, 300U, 500U, 1U);
    EnvAlarm_Mute(&state);
    reading(&state, 300U, 700U, 1U);
    CHECK(state.muted == 0U);
    CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY));
    EnvAlarm_Mute(&state);
    reading(&state, 0U, 0U, 0U);
    CHECK(state.muted == 0U);
    CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY | ENV_ALARM_FLAG_SENSOR));
    EnvAlarm_Mute(&state);
    reading(&state, 0U, 0U, 0U);
    CHECK(state.muted == 1U);
    reading(&state, 250U, 500U, 1U);
    CHECK(state.flags == 0U && state.muted == 0U);
    return 0U;
}

static uint8_t text_matches(const char *left, const char *right)
{
    while ((*left != '\0') && (*left == *right))
    {
        ++left;
        ++right;
    }
    return (*left == *right) ? 1U : 0U;
}

static uint32_t test_fault_and_text(void)
{
    EnvAlarm_State state;
    EnvAlarm_Init(&state);
    CHECK(text_matches(EnvAlarm_StatusText(&state), "NORMAL"));
    reading(&state, 500U, 999U, 0U);
    CHECK(state.flags == ENV_ALARM_FLAG_SENSOR);
    CHECK(text_matches(EnvAlarm_StatusText(&state), "SENSOR FAULT"));
    reading(&state, 290U, 680U, 1U);
    CHECK(state.flags == 0U);
    reading(&state, 300U, 500U, 1U);
    CHECK(text_matches(EnvAlarm_StatusText(&state), "TEMP HIGH"));
    reading(&state, 250U, 700U, 1U);
    CHECK(text_matches(EnvAlarm_StatusText(&state), "HUM HIGH"));
    reading(&state, 300U, 700U, 1U);
    CHECK(text_matches(EnvAlarm_StatusText(&state), "TEMP+HUM HIGH"));
    return 0U;
}

static uint32_t test_button_bounce(void)
{
    test_ms = 0U;
    test_button_level = GPIO_PIN_SET;
    Button_Init();
    CHECK(button_step(1U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(5U, GPIO_PIN_SET) == BUTTON_EVENT_NONE);
    CHECK(button_step(8U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(27U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(28U, GPIO_PIN_RESET) == BUTTON_EVENT_PRESSED);
    CHECK(button_step(50U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(500U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(501U, GPIO_PIN_SET) == BUTTON_EVENT_NONE);
    CHECK(button_step(510U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(515U, GPIO_PIN_SET) == BUTTON_EVENT_NONE);
    CHECK(button_step(534U, GPIO_PIN_SET) == BUTTON_EVENT_NONE);
    CHECK(button_step(535U, GPIO_PIN_SET) == BUTTON_EVENT_RELEASED);
    CHECK(button_step(600U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(620U, GPIO_PIN_RESET) == BUTTON_EVENT_PRESSED);
    return 0U;
}

static uint32_t test_button_held_at_boot(void)
{
    test_ms = 0U;
    test_button_level = GPIO_PIN_RESET;
    Button_Init();
    CHECK(button_step(100U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(200U, GPIO_PIN_SET) == BUTTON_EVENT_NONE);
    CHECK(button_step(220U, GPIO_PIN_SET) == BUTTON_EVENT_RELEASED);
    CHECK(button_step(300U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(320U, GPIO_PIN_RESET) == BUTTON_EVENT_PRESSED);
    return 0U;
}

static uint32_t test_tick_rollover(void)
{
    test_ms = 0xFFFFFFF0U;
    test_button_level = GPIO_PIN_SET;
    Button_Init();
    CHECK(button_step(0xFFFFFFF8U, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(0x0000000BU, GPIO_PIN_RESET) == BUTTON_EVENT_NONE);
    CHECK(button_step(0x0000000CU, GPIO_PIN_RESET) == BUTTON_EVENT_PRESSED);
    CHECK(button_step(0x0000000DU, GPIO_PIN_SET) == BUTTON_EVENT_NONE);
    CHECK(button_step(0x00000021U, GPIO_PIN_SET) == BUTTON_EVENT_RELEASED);
    return 0U;
}

static uint32_t test_output_polarity(void)
{
    wrong_gpio_access = 0U;
    LED_Init();
    Buzzer_Init();
    CHECK(test_led_level == GPIO_PIN_RESET);
    CHECK(test_buzzer_level == GPIO_PIN_SET);
    LED_Set(1U);
    CHECK(test_led_level == GPIO_PIN_SET && test_buzzer_level == GPIO_PIN_SET);
    Buzzer_Set(1U);
    CHECK(test_buzzer_level == GPIO_PIN_RESET && test_led_level == GPIO_PIN_SET);
    LED_Set(0U);
    Buzzer_Set(0U);
    CHECK(test_led_level == GPIO_PIN_RESET && test_buzzer_level == GPIO_PIN_SET);
    CHECK(wrong_gpio_access == 0U);
    return 0U;
}

static uint32_t test_runtime_temperature(void)
{
    EnvAlarm_State state;
    EnvAlarm_Request request;
    EnvAlarm_Response response;
    EnvAlarm_Sample sample;
    EnvAlarm_Init(&state);
    sample.temperature_x10 = 290U;
    sample.humidity_x10 = 500U;
    sample.valid = 1U;
    request.request_id = 7U;
    request.kind = ENV_ALARM_REQUEST_SET_TEMPERATURE;
    request.temperature_x10 = 290U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(response.request_id == 7U && response.result == ENV_ALARM_REQUEST_OK);
    CHECK(state.config.temperature_on_x10 == 290U && state.config.temperature_off_x10 == 270U);
    CHECK(state.flags == ENV_ALARM_FLAG_TEMPERATURE && state.muted == 0U);
    EnvAlarm_Mute(&state);
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(state.muted == 1U); /* Same continuing cause stays muted. */
    request.temperature_x10 = 320U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(state.flags == 0U && state.muted == 0U);
    request.temperature_x10 = 290U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    reading(&state, 271U, 500U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_TEMPERATURE);
    reading(&state, 270U, 500U, 1U);
    CHECK(state.flags == 0U);
    return 0U;
}

static uint32_t test_runtime_boundaries(void)
{
    EnvAlarm_State state;
    EnvAlarm_Request request;
    EnvAlarm_Response response;
    EnvAlarm_Init(&state);
    request.request_id = 1U;
    request.kind = ENV_ALARM_REQUEST_SET_HUMIDITY;
    request.humidity_x10 = 50U;
    EnvAlarm_ApplyRequest(&state, &request, 0, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK);
    CHECK(state.config.humidity_on_x10 == 50U && state.config.humidity_off_x10 == 0U);
    reading(&state, 0U, 50U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_HUMIDITY);
    reading(&state, 0U, 1U, 1U);
    CHECK(state.flags == ENV_ALARM_FLAG_HUMIDITY);
    reading(&state, 0U, 0U, 1U);
    CHECK(state.flags == 0U);
    request.humidity_x10 = 1000U;
    EnvAlarm_ApplyRequest(&state, &request, 0, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK && state.config.humidity_off_x10 == 950U);
    request.kind = ENV_ALARM_REQUEST_SET_TEMPERATURE;
    request.temperature_x10 = 20U;
    EnvAlarm_ApplyRequest(&state, &request, 0, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK && state.config.temperature_off_x10 == 0U);
    request.temperature_x10 = 500U;
    EnvAlarm_ApplyRequest(&state, &request, 0, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK && state.config.temperature_off_x10 == 480U);
    return 0U;
}

static uint32_t test_runtime_rejection(void)
{
    EnvAlarm_State state;
    EnvAlarm_Request request;
    EnvAlarm_Response response;
    EnvAlarm_Sample sample;
    uint16_t values[4] = {19U, 501U, 49U, 1001U};
    uint8_t index;
    EnvAlarm_Init(&state);
    reading(&state, 310U, 710U, 1U);
    EnvAlarm_Mute(&state);
    sample.temperature_x10 = 0U;
    sample.humidity_x10 = 0U;
    sample.valid = 1U;
    request.request_id = 8U;
    for (index = 0U; index < 4U; ++index)
    {
        request.kind = (index < 2U) ? ENV_ALARM_REQUEST_SET_TEMPERATURE : ENV_ALARM_REQUEST_SET_HUMIDITY;
        request.temperature_x10 = values[index];
        request.humidity_x10 = values[index];
        EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
        CHECK(response.result == ENV_ALARM_REQUEST_RANGE_ERROR);
        CHECK(response.config.temperature_on_x10 == ENV_ALARM_TEMP_ON_X10);
        CHECK(state.config.temperature_off_x10 == ENV_ALARM_TEMP_OFF_X10);
        CHECK(state.config.humidity_on_x10 == ENV_ALARM_HUM_ON_X10);
        CHECK(state.config.humidity_off_x10 == ENV_ALARM_HUM_OFF_X10);
        CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY));
        CHECK(state.muted == 1U);
    }
    request.kind = (EnvAlarm_RequestKind)99;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_INVALID && state.muted == 1U);
    return 0U;
}

static uint32_t test_runtime_no_sample_and_fault(void)
{
    EnvAlarm_State state;
    EnvAlarm_Request request;
    EnvAlarm_Response response;
    EnvAlarm_Sample sample;
    EnvAlarm_Init(&state);
    request.request_id = 1U;
    request.kind = ENV_ALARM_REQUEST_SET_TEMPERATURE;
    request.temperature_x10 = 350U;
    EnvAlarm_ApplyRequest(&state, &request, 0, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK && state.flags == 0U);
    reading(&state, 350U, 500U, 1U);
    EnvAlarm_Mute(&state);
    sample.valid = 0U;
    sample.temperature_x10 = 0U;
    sample.humidity_x10 = 0U;
    request.temperature_x10 = 400U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_SENSOR));
    CHECK(state.muted == 0U);
    EnvAlarm_Init(&state);
    CHECK(state.flags == 0U && state.muted == 0U);
    CHECK(state.config.temperature_on_x10 == ENV_ALARM_TEMP_ON_X10);
    CHECK(state.config.temperature_off_x10 == ENV_ALARM_TEMP_OFF_X10);
    CHECK(state.config.humidity_on_x10 == ENV_ALARM_HUM_ON_X10);
    CHECK(state.config.humidity_off_x10 == ENV_ALARM_HUM_OFF_X10);
    return 0U;
}

static uint32_t test_combined_commit_and_reject(void)
{
    EnvAlarm_State state;
    EnvAlarm_Request request;
    EnvAlarm_Response response;
    EnvAlarm_Sample sample;
    uint8_t index;
    uint16_t bad_temperature[4] = {19U, 501U, 350U, 350U};
    uint16_t bad_humidity[4] = {800U, 800U, 49U, 1001U};
    EnvAlarm_Init(&state);
    request.request_id = 100U;
    request.kind = ENV_ALARM_REQUEST_SET_BOTH;
    request.temperature_x10 = 350U;
    request.humidity_x10 = 800U;
    EnvAlarm_ApplyRequest(&state, &request, 0, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK && response.request_id == 100U);
    CHECK(response.kind == ENV_ALARM_REQUEST_SET_BOTH);
    CHECK(state.config.temperature_on_x10 == 350U && state.config.temperature_off_x10 == 330U);
    CHECK(state.config.humidity_on_x10 == 800U && state.config.humidity_off_x10 == 750U);
    CHECK(state.flags == 0U);
    reading(&state, 360U, 850U, 1U);
    EnvAlarm_Mute(&state);
    sample.temperature_x10 = 0U;
    sample.humidity_x10 = 0U;
    sample.valid = 1U;
    for (index = 0U; index < 4U; ++index)
    {
        request.temperature_x10 = bad_temperature[index];
        request.humidity_x10 = bad_humidity[index];
        EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
        CHECK(response.result == ENV_ALARM_REQUEST_RANGE_ERROR);
        CHECK(state.config.temperature_on_x10 == 350U && state.config.temperature_off_x10 == 330U);
        CHECK(state.config.humidity_on_x10 == 800U && state.config.humidity_off_x10 == 750U);
        CHECK(response.config.temperature_on_x10 == 350U && response.config.humidity_on_x10 == 800U);
        CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY));
        CHECK(state.muted == 1U); /* Rejected settings must not reevaluate. */
    }
    return 0U;
}

static uint32_t test_combined_single_reevaluation(void)
{
    EnvAlarm_State state;
    EnvAlarm_Request request;
    EnvAlarm_Response response;
    EnvAlarm_Sample sample;
    EnvAlarm_Init(&state);
    reading(&state, 350U, 750U, 1U);
    EnvAlarm_Mute(&state);
    request.request_id = 1U;
    request.kind = ENV_ALARM_REQUEST_SET_BOTH;
    request.temperature_x10 = 400U;
    request.humidity_x10 = 720U;
    sample.temperature_x10 = 350U;
    sample.humidity_x10 = 750U;
    sample.valid = 1U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(state.flags == ENV_ALARM_FLAG_HUMIDITY && state.muted == 1U);
    /* Humidity remains an existing cause under the final settings;
     * removing temperature alone must preserve mute. */
    request.temperature_x10 = 400U;
    request.humidity_x10 = 800U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(state.flags == 0U && state.muted == 0U);
    request.temperature_x10 = 340U;
    request.humidity_x10 = 740U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY) && state.muted == 0U);
    reading(&state, 321U, 691U, 1U);
    CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY));
    reading(&state, 320U, 690U, 1U);
    CHECK(state.flags == 0U);
    return 0U;
}

static uint32_t test_combined_limits_and_fault(void)
{
    EnvAlarm_State state;
    EnvAlarm_Request request;
    EnvAlarm_Response response;
    EnvAlarm_Sample sample;
    EnvAlarm_Init(&state);
    request.request_id = 1U;
    request.kind = ENV_ALARM_REQUEST_SET_BOTH;
    request.temperature_x10 = 20U;
    request.humidity_x10 = 50U;
    EnvAlarm_ApplyRequest(&state, &request, 0, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK);
    CHECK(state.config.temperature_off_x10 == 0U && state.config.humidity_off_x10 == 0U);
    reading(&state, 20U, 50U, 1U);
    EnvAlarm_Mute(&state);
    sample.temperature_x10 = 0U;
    sample.humidity_x10 = 0U;
    sample.valid = 0U;
    request.temperature_x10 = 500U;
    request.humidity_x10 = 1000U;
    EnvAlarm_ApplyRequest(&state, &request, &sample, &response);
    CHECK(response.result == ENV_ALARM_REQUEST_OK);
    CHECK(state.config.temperature_off_x10 == 480U && state.config.humidity_off_x10 == 950U);
    CHECK(state.flags == (ENV_ALARM_FLAG_TEMPERATURE | ENV_ALARM_FLAG_HUMIDITY | ENV_ALARM_FLAG_SENSOR));
    CHECK(state.muted == 0U);
    return 0U;
}

uint32_t RunAlarmDriverTests(void)
{
    uint32_t failures = 0U;
    failures |= test_hysteresis() << 0;
    failures |= test_mute_recovery() << 1;
    failures |= test_new_cause() << 2;
    failures |= test_fault_and_text() << 3;
    failures |= test_button_bounce() << 4;
    failures |= test_button_held_at_boot() << 5;
    failures |= test_tick_rollover() << 6;
    failures |= test_output_polarity() << 7;
    failures |= test_runtime_temperature() << 8;
    failures |= test_runtime_boundaries() << 9;
    failures |= test_runtime_rejection() << 10;
    failures |= test_runtime_no_sample_and_fault() << 11;
    failures |= test_combined_commit_and_reject() << 12;
    failures |= test_combined_single_reevaluation() << 13;
    failures |= test_combined_limits_and_fault() << 14;
    return failures;
}
