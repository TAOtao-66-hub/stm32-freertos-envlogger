#include "board_timing.h"
#include "main.h"

static uint32_t cycles_per_us;

uint8_t BoardTiming_Init(void)
{
    uint32_t before;
    volatile uint32_t i;

    cycles_per_us = HAL_RCC_GetHCLKFreq() / 1000000U;
    if ((cycles_per_us == 0U) || ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U)) {
        return 0U;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    before = DWT->CYCCNT;
    for (i = 0U; i < 32U; ++i) {
        __NOP();
    }
    return (uint8_t)(DWT->CYCCNT != before);
}

uint32_t BoardTiming_Cycles(void)
{
    return DWT->CYCCNT;
}

uint32_t BoardTiming_ElapsedUs(uint32_t start_cycles)
{
    /* Unsigned subtraction also handles a CYCCNT wrap during a short pulse. */
    return (DWT->CYCCNT - start_cycles) / cycles_per_us;
}

uint32_t BoardTiming_CyclesToUs(uint32_t cycles)
{
    return cycles / cycles_per_us;
}

void BoardTiming_DelayUs(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = us * cycles_per_us;
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
}
