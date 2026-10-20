#include "led.h"
#include "gpio.h"

void LED_Init(void)
{
    LED_Set(0U);
}

void LED_Set(uint8_t enabled)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin,
                      (enabled != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
