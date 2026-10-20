#ifndef LED_H
#define LED_H

#include <stdint.h>

/* MX_GPIO_Init must run first. The LED anode connects to PA4 via a resistor. */
void LED_Init(void);
void LED_Set(uint8_t enabled);

#endif /* LED_H */
