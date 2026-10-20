#ifndef GPIO_TEST_HAL_H
#define GPIO_TEST_HAL_H
#include <stdint.h>

typedef struct { uint32_t index; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpioa, test_gpiob;
#define GPIOA (&test_gpioa)
#define GPIOB (&test_gpiob)
typedef enum { GPIO_PIN_RESET = 0U, GPIO_PIN_SET } GPIO_PinState;
typedef struct { uint32_t Pin, Mode, Pull, Speed; } GPIO_InitTypeDef;
#define GPIO_PIN_0 (1U << 0)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_MODE_ANALOG 5U
#define GPIO_PIN_2  (1U << 2)
#define GPIO_PIN_3  (1U << 3)
#define GPIO_PIN_4  (1U << 4)
#define GPIO_PIN_6  (1U << 6)
#define GPIO_PIN_7  (1U << 7)
#define GPIO_PIN_9  (1U << 9)
#define GPIO_PIN_10 (1U << 10)
#define GPIO_MODE_INPUT 1U
#define GPIO_MODE_OUTPUT_PP 2U
#define GPIO_MODE_OUTPUT_OD 3U
#define GPIO_MODE_AF_PP 4U
#define GPIO_NOPULL 0U
#define GPIO_PULLUP 1U
#define GPIO_SPEED_FREQ_LOW 2U
#define GPIO_SPEED_FREQ_HIGH 3U
#define __HAL_RCC_GPIOA_CLK_ENABLE() test_enable_clock(0U)
#define __HAL_RCC_GPIOB_CLK_ENABLE() test_enable_clock(1U)
void test_enable_clock(uint32_t port);
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *config);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pins, GPIO_PinState state);
#endif
