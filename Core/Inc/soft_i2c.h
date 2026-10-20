#ifndef SOFT_I2C_H
#define SOFT_I2C_H

#include <stdint.h>

/* PB6=SCL, PB7=SDA. Both are GPIO open-drain, with EXTERNAL 3.3 V pull-ups.
 * Call from one task only; add a bus mutex if another task is added later. */
uint8_t SoftI2C_Init(void);
uint8_t SoftI2C_Probe(uint8_t address_7bit);
uint8_t SoftI2C_Write(uint8_t address_7bit, uint8_t control,
                      const uint8_t *data, uint16_t length);

#endif
