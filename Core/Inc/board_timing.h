#ifndef BOARD_TIMING_H
#define BOARD_TIMING_H

#include <stdint.h>

/* Cortex-M3 DWT time base; independent of the TIM2 HAL tick and SysTick. */
uint8_t BoardTiming_Init(void);
uint32_t BoardTiming_Cycles(void);
uint32_t BoardTiming_CyclesToUs(uint32_t cycles);
uint32_t BoardTiming_ElapsedUs(uint32_t start_cycles);
void BoardTiming_DelayUs(uint32_t us);

#endif
