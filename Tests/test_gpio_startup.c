#include "gpio.h"

GPIO_TypeDef test_gpioa = {0U};
GPIO_TypeDef test_gpiob;
static GPIO_InitTypeDef pin_config[2][16];
static uint16_t output_level[2];
static uint16_t configured[2];
static uint16_t preloaded[2];
static uint32_t clock_enabled[2];
static uint32_t order_errors;
static uint32_t tx_configuration_count;

void test_enable_clock(uint32_t port) { clock_enabled[port] = 1U; }

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pins, GPIO_PinState state)
{
    if (clock_enabled[port->index] == 0U) { ++order_errors; }
    preloaded[port->index] |= pins;
    if (state == GPIO_PIN_SET) { output_level[port->index] |= pins; }
    else { output_level[port->index] &= (uint16_t)~pins; }
}

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *config)
{
    uint32_t pin;
    uint32_t index = port->index;

    if (clock_enabled[index] == 0U) { ++order_errors; }
    if ((config->Mode != GPIO_MODE_INPUT) && (config->Mode != GPIO_MODE_ANALOG) &&
        ((preloaded[index] & config->Pin) != config->Pin)) { ++order_errors; }
    for (pin = 0U; pin < 16U; ++pin)
    {
        if ((config->Pin & (1U << pin)) != 0U)
        {
            pin_config[index][pin] = *config;
            configured[index] |= (uint16_t)(1U << pin);
        }
    }
    if ((port == GPIOA) && ((config->Pin & GPIO_PIN_9) != 0U)) { ++tx_configuration_count; }
}

uint32_t RunGPIOStartupTests(void)
{
    uint32_t failures = 0U;

    /* The emulator enters this function directly, without the C startup copy. */
    test_gpiob.index = 1U;
    MX_GPIO_Init();
    if ((order_errors != 0U) || (output_level[0] != (GPIO_PIN_2 | GPIO_PIN_9)) ||
        (output_level[1] != (GPIO_PIN_0 | GPIO_PIN_6 | GPIO_PIN_7))) { failures |= 1U; }
    if ((pin_config[0][2].Mode != GPIO_MODE_OUTPUT_PP) ||
        (pin_config[0][3].Mode != GPIO_MODE_INPUT) || (pin_config[0][3].Pull != GPIO_PULLUP) ||
        (pin_config[0][4].Mode != GPIO_MODE_OUTPUT_PP) ||
        (pin_config[1][6].Mode != GPIO_MODE_OUTPUT_OD) ||
        (pin_config[1][7].Mode != GPIO_MODE_OUTPUT_OD) ||
        (pin_config[1][10].Mode != GPIO_MODE_INPUT) || (pin_config[1][10].Pull != GPIO_PULLUP))
    { failures |= 2U; }
    if ((pin_config[0][9].Mode != GPIO_MODE_OUTPUT_PP) || (tx_configuration_count != 1U) ||
        (pin_config[0][10].Mode != GPIO_MODE_INPUT) || (pin_config[0][10].Pull != GPIO_PULLUP))
    { failures |= 4U; }
    MX_GPIO_USART1_TX_Init();
    if ((pin_config[0][9].Mode != GPIO_MODE_AF_PP) || (tx_configuration_count != 2U) ||
        (order_errors != 0U) || (configured[0] & ((1U << 13) | (1U << 14))) != 0U)
    { failures |= 8U; }
    if ((pin_config[0][0].Mode != GPIO_MODE_ANALOG) ||
        (pin_config[0][5].Mode != GPIO_MODE_AF_PP) ||
        (pin_config[0][6].Mode != GPIO_MODE_INPUT) ||
        (pin_config[0][6].Pull != GPIO_NOPULL) ||
        (pin_config[0][7].Mode != GPIO_MODE_AF_PP) ||
        (pin_config[1][0].Mode != GPIO_MODE_OUTPUT_PP)) { failures |= 16U; }
    return failures;
}
