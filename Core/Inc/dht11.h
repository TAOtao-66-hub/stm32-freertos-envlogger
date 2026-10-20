#ifndef DHT11_H
#define DHT11_H

#include "dht11_decoder.h"

typedef enum {
    DHT11_OK = 0,
    DHT11_WAIT,
    DHT11_TIMEOUT,
    DHT11_CHECKSUM_ERROR,
    DHT11_PROTOCOL_ERROR,
    DHT11_BUS_LOW,
    DHT11_TIMING_ERROR
} DHT11_Status;

/* PB10: idle input/pull-up; host start open-drain low; receive EXTI input.
 * Init once, then call Read from ONE RTOS task.
 * Sampling period and minimum read interval are together in project_config.h.
 * WAIT means no acquisition was started; it is not a sensor failure.
 * Reading is only modified on success. EXTI priority 4 uses NO RTOS APIs. */
uint8_t DHT11_Init(void);
DHT11_Status DHT11_Read(DHT11_Reading *reading);
void DHT11_EXTI_IRQHandler(void);
const char *DHT11_StatusText(DHT11_Status status);

#endif
