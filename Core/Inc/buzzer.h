#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>

/* Verified active buzzer module: PA2 LOW = sound, HIGH = silence; no PWM. */
/* MX_GPIO_Init must run first. This API is owned by the alarm task. */
void Buzzer_Init(void);
void Buzzer_Set(uint8_t enabled);

#endif /* BUZZER_H */
