#include "time_service.h"
#include "main.h"
#include "project_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

static uint8_t s_ready;
static uint8_t s_calibrated;
static uint32_t s_last_tick;
static uint32_t s_uptime;
static uint32_t s_remainder_ms;

static uint8_t wait_register(uint32_t mask)
{
    uint32_t start = HAL_GetTick();
    while ((RTC->CRL & mask) == 0U)
    {
        if ((uint32_t)(HAL_GetTick() - start) >= TIME_RTC_TIMEOUT_MS) { return 0U; }
    }
    return 1U;
}

uint8_t TimeService_Init(void)
{
    s_ready = 0U;
    s_calibrated = 0U; /* Every firmware startup explicitly requires PC calibration. */
    s_last_tick = HAL_GetTick();
    s_uptime = 0U;
    s_remainder_ms = 0U;
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    /* SystemClock_Config enables LSI and selects it for RTC in .ioc as well. */
    __HAL_RCC_RTC_ENABLE();
    RTC->CRL &= ~RTC_CRL_RSF;
    if ((wait_register(RTC_CRL_RSF) == 0U) || (wait_register(RTC_CRL_RTOFF) == 0U)) { return 0U; }
    RTC->CRL |= RTC_CRL_CNF;
    RTC->PRLH = 0U;
    RTC->PRLL = LSI_VALUE - 1U;
    RTC->CRL &= ~RTC_CRL_CNF;
    s_ready = wait_register(RTC_CRL_RTOFF);
    return s_ready;
}

uint8_t TimeService_SetText(const char *text)
{
    uint32_t timestamp;
    if (TimeService_Parse(text, &timestamp) == 0U) { return 0U; }
    return TimeService_SetUnix(timestamp);
}

uint8_t TimeService_SetUnix(uint32_t timestamp)
{
    /* 2020-01-01 00:00:00 .. 2099-12-31 23:59:59 in Beijing time. */
    if ((s_ready == 0U) || (timestamp < 1577808000UL) || (timestamp > 4102415999UL) ||
        (wait_register(RTC_CRL_RTOFF) == 0U)) { return 0U; }
    taskENTER_CRITICAL();
    s_calibrated = 0U;
    RTC->CRL |= RTC_CRL_CNF;
    RTC->CNTH = timestamp >> 16U;
    RTC->CNTL = timestamp & 0xFFFFU;
    RTC->CRL &= ~RTC_CRL_CNF;
    taskEXIT_CRITICAL();
    if (wait_register(RTC_CRL_RTOFF) == 0U) { return 0U; }
    taskENTER_CRITICAL();
    s_calibrated = 1U;
    taskEXIT_CRITICAL();
    return 1U;
}

void TimeService_Get(TimeService_Snapshot *snapshot)
{
    uint32_t now;
    uint32_t elapsed;
    uint32_t high;
    uint32_t low;

    taskENTER_CRITICAL();
    now = HAL_GetTick();
    elapsed = now - s_last_tick;
    s_last_tick = now;
    s_uptime += elapsed / 1000U;
    s_remainder_ms += elapsed % 1000U;
    s_uptime += s_remainder_ms / 1000U;
    s_remainder_ms %= 1000U;
    do { high = RTC->CNTH; low = RTC->CNTL; } while (high != RTC->CNTH);
    snapshot->timestamp = (high << 16U) | low;
    snapshot->uptime_seconds = s_uptime;
    snapshot->calibrated = s_calibrated;
    taskEXIT_CRITICAL();
}
