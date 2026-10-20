#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "queue.h"

/* USART1 on PA9/PA10. Call Init after MX_GPIO_Init and before task creation.
 * After the scheduler starts, ONE task owns StartReceive, Service and writes.
 * RX queue items must be uint8_t. Keep the queue alive for the driver lifetime.
 * UART handle and RX working buffer are private to serial_port.c. */
typedef struct
{
    uint32_t rx_received_count;
    uint32_t rx_dropped_count;
    uint32_t rx_error_count;
    uint32_t tx_error_count;
    uint8_t rx_recovery_required;
} SerialPort_Stats;

HAL_StatusTypeDef SerialPort_Init(void);
HAL_StatusTypeDef SerialPort_StartReceive(QueueHandle_t rx_queue);
/* Task-only: retry RX after UART errors or failure to rearm the interrupt. */
HAL_StatusTypeDef SerialPort_Service(void);
/* Task-only blocking TX, bounded by SERIAL_TX_MAX_BYTES / SERIAL_TX_TIMEOUT_MS.
 * Return HAL_OK, HAL_ERROR, HAL_BUSY or HAL_TIMEOUT. Never call from an ISR. */
HAL_StatusTypeDef SerialPort_Write(const uint8_t *data, uint16_t length);
HAL_StatusTypeDef SerialPort_WriteString(const char *text);
/* Task-only coherent diagnostic snapshot; useful for Keil Watch. */
void SerialPort_GetStats(SerialPort_Stats *stats);
/* Called only by USART1_IRQHandler in stm32f1xx_it.c. */
void SerialPort_IRQHandler(void);

#endif /* SERIAL_PORT_H */
