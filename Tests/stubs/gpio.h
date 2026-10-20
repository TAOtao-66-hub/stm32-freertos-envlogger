#ifndef TEST_GPIO_H
#define TEST_GPIO_H

#include <stdint.h>

/* Test-only HAL boundary; the production build uses Core/Inc/gpio.h. */
typedef struct
{
    uint32_t unused;
} GPIO_TypeDef;

typedef enum
{
    GPIO_PIN_RESET = 0,
    GPIO_PIN_SET
} GPIO_PinState;

extern GPIO_TypeDef test_gpio_a;
#define BUZZER_GPIO_Port (&test_gpio_a)
#define BUTTON_GPIO_Port (&test_gpio_a)
#define LED_GPIO_Port    (&test_gpio_a)
#define BUZZER_Pin       ((uint16_t)0x0004U)
#define BUTTON_Pin       ((uint16_t)0x0008U)
#define LED_Pin          ((uint16_t)0x0010U)

uint32_t HAL_GetTick(void);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState level);

#endif /* TEST_GPIO_H */
