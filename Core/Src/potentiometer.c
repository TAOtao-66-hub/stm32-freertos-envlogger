#include "potentiometer.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

static ADC_HandleTypeDef s_adc;
static Pot_Snapshot s_snapshot;
static uint32_t s_last_poll;
static int32_t s_filter_q8;
static uint16_t s_last_value;
static uint8_t s_filter_valid;
static uint8_t s_force_apply;

uint8_t Pot_Init(void)
{
    ADC_ChannelConfTypeDef channel = {0};
    __HAL_RCC_ADC1_CLK_ENABLE();
    s_adc.Instance = ADC1;
    s_adc.Init.ScanConvMode = ADC_SCAN_DISABLE;
    s_adc.Init.ContinuousConvMode = DISABLE;
    s_adc.Init.DiscontinuousConvMode = DISABLE;
    s_adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    s_adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    s_adc.Init.NbrOfConversion = 1U;
    channel.Channel = ADC_CHANNEL_0;
    channel.Rank = ADC_REGULAR_RANK_1;
    channel.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    s_snapshot.ready = ((HAL_ADC_Init(&s_adc) == HAL_OK) &&
                         (HAL_ADC_ConfigChannel(&s_adc, &channel) == HAL_OK) &&
                         (HAL_ADCEx_Calibration_Start(&s_adc) == HAL_OK)) ? 1U : 0U;
    s_snapshot.mode = POT_MODE_OFF;
    s_filter_valid = 0U;
    return s_snapshot.ready;
}

uint8_t Pot_SetMode(Pot_Mode mode)
{
    if ((mode > POT_MODE_HUMIDITY) || ((mode != POT_MODE_OFF) && (s_snapshot.ready == 0U))) { return 0U; }
    taskENTER_CRITICAL();
    s_snapshot.mode = mode;
    taskEXIT_CRITICAL();
    s_force_apply = 1U;
    return 1U;
}

uint8_t Pot_Poll(uint32_t now_ms, EnvAlarm_Request *request)
{
    uint16_t raw;
    uint16_t value;
    uint16_t minimum;
    uint16_t maximum;
    uint16_t step;
    uint16_t difference;
    Pot_Mode mode = s_snapshot.mode;

    if ((s_snapshot.ready == 0U) || ((uint32_t)(now_ms - s_last_poll) < POT_SAMPLE_INTERVAL_MS)) { return 0U; }
    s_last_poll = now_ms;
    if (HAL_ADC_Start(&s_adc) != HAL_OK) { return 0U; }
    if (HAL_ADC_PollForConversion(&s_adc, POT_ADC_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_adc);
        return 0U;
    }
    raw = (uint16_t)HAL_ADC_GetValue(&s_adc);
    (void)HAL_ADC_Stop(&s_adc);
    if (s_filter_valid == 0U) { s_filter_q8 = (int32_t)raw * 256; s_filter_valid = 1U; }
    else { s_filter_q8 += ((int32_t)raw * 256 - s_filter_q8) / 8; }
    taskENTER_CRITICAL();
    s_snapshot.raw = raw;
    s_snapshot.filtered = (uint16_t)(s_filter_q8 / 256);
    taskEXIT_CRITICAL();
    if (mode == POT_MODE_OFF) { return 0U; }
    minimum = (mode == POT_MODE_TEMPERATURE) ? ENV_ALARM_TEMP_HYSTERESIS_X10 : ENV_ALARM_HUM_HYSTERESIS_X10;
    maximum = (mode == POT_MODE_TEMPERATURE) ? ENV_ALARM_TEMP_SET_MAX_X10 : ENV_ALARM_HUM_SET_MAX_X10;
    step = (mode == POT_MODE_TEMPERATURE) ? POT_TEMPERATURE_STEP_X10 : POT_HUMIDITY_STEP_X10;
    value = (uint16_t)(minimum + ((uint32_t)s_snapshot.filtered * (maximum - minimum) + 2047U) / 4095U);
    value = (uint16_t)(((uint32_t)value + step / 2U) / step * step);
    if (value < minimum) { value = minimum; }
    if (value > maximum) { value = maximum; }
    difference = (value > s_last_value) ? value - s_last_value : s_last_value - value;
    if ((s_force_apply == 0U) && (difference < 2U * step) && (value != minimum) && (value != maximum)) { return 0U; }
    if ((s_force_apply == 0U) && (value == s_last_value)) { return 0U; }
    s_force_apply = 0U;
    s_last_value = value;
    request->request_id = 0U;
    request->kind = (mode == POT_MODE_TEMPERATURE) ? ENV_ALARM_REQUEST_SET_TEMPERATURE : ENV_ALARM_REQUEST_SET_HUMIDITY;
    request->temperature_x10 = value;
    request->humidity_x10 = value;
    return 1U;
}

void Pot_Get(Pot_Snapshot *snapshot)
{
    taskENTER_CRITICAL();
    *snapshot = s_snapshot;
    taskEXIT_CRITICAL();
}
