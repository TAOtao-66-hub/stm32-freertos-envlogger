#include "buzzer.h"
#include "gpio.h"

void Buzzer_Init(void)
{
    Buzzer_Set(0U);
}

void Buzzer_Set(uint8_t enabled)
{
    HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin,
                      (enabled != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}
