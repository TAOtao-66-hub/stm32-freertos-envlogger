#include "button.h"
#include "gpio.h"
#include "project_config.h"

/* Private types ----------------------------------------------------------- */
typedef struct
{
    GPIO_PinState last_raw_level;
    GPIO_PinState stable_level;
    uint32_t raw_changed_ms;
    uint8_t press_armed;
} Button_Context;

/* Private variables: owned exclusively by alarmTask. ---------------------- */
static Button_Context s_button;

/* Public functions -------------------------------------------------------- */

void Button_Init(void)
{
    s_button.last_raw_level = HAL_GPIO_ReadPin(BUTTON_GPIO_Port, BUTTON_Pin);
    s_button.stable_level = s_button.last_raw_level;
    s_button.raw_changed_ms = HAL_GetTick();
    s_button.press_armed = (s_button.stable_level == GPIO_PIN_SET) ? 1U : 0U;
}

Button_Event Button_Poll(void)
{
    uint32_t now = HAL_GetTick();
    GPIO_PinState raw_level = HAL_GPIO_ReadPin(BUTTON_GPIO_Port, BUTTON_Pin);

    if (raw_level != s_button.last_raw_level)
    {
        s_button.last_raw_level = raw_level;
        s_button.raw_changed_ms = now;
    }

    /* Unsigned subtraction also handles the 32-bit millisecond tick rollover. */
    if ((raw_level == s_button.stable_level) ||
        ((uint32_t)(now - s_button.raw_changed_ms) < BUTTON_DEBOUNCE_MS))
    {
        return BUTTON_EVENT_NONE;
    }

    s_button.stable_level = raw_level;
    if (s_button.stable_level == GPIO_PIN_SET)
    {
        s_button.press_armed = 1U;
        return BUTTON_EVENT_RELEASED;
    }

    if (s_button.press_armed != 0U)
    {
        s_button.press_armed = 0U;
        return BUTTON_EVENT_PRESSED;
    }

    return BUTTON_EVENT_NONE;
}
