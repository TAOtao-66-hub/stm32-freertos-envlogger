#include "serial_port.h"
#include "project_config.h"
#include "task.h"

/* Private types ----------------------------------------------------------- */
typedef struct
{
    UART_HandleTypeDef uart;
    QueueHandle_t rx_queue;
    uint8_t rx_byte;
    uint8_t initialized;
    volatile uint8_t rx_recovery_required;
    volatile uint32_t rx_received_count;
    volatile uint32_t rx_dropped_count;
    volatile uint32_t rx_error_count;
    uint32_t tx_error_count;
} SerialPort_Context;

/* Private variables: RX capture is shared with the USART1 ISR. ------------- */
static SerialPort_Context s_serial_port;

/* Public functions -------------------------------------------------------- */
HAL_StatusTypeDef SerialPort_Init(void)
{
    HAL_StatusTypeDef status;

    if (s_serial_port.initialized != 0U)
    {
        return HAL_OK;
    }

    s_serial_port.uart.Instance = USART1;
    s_serial_port.uart.Init.BaudRate = SERIAL_BAUD_RATE;
    s_serial_port.uart.Init.WordLength = UART_WORDLENGTH_8B;
    s_serial_port.uart.Init.StopBits = UART_STOPBITS_1;
    s_serial_port.uart.Init.Parity = UART_PARITY_NONE;
    s_serial_port.uart.Init.Mode = UART_MODE_TX_RX;
    s_serial_port.uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    s_serial_port.uart.Init.OverSampling = UART_OVERSAMPLING_16;

    /* MSP enables the USART1 clock and sets its IRQ priority, not the GPIOs.
     * The NVIC line stays disabled until StartReceive runs in a task. */
    status = HAL_UART_Init(&s_serial_port.uart);
    if (status == HAL_OK)
    {
        s_serial_port.initialized = 1U;
    }
    return status;
}

HAL_StatusTypeDef SerialPort_StartReceive(QueueHandle_t rx_queue)
{
    HAL_StatusTypeDef status;

    if ((s_serial_port.initialized == 0U) || (rx_queue == NULL))
    {
        return HAL_ERROR;
    }
    if (s_serial_port.rx_queue != NULL)
    {
        return HAL_BUSY;
    }

    /* Discard stale startup data/error flags before arming a fresh byte. */
    __HAL_UART_CLEAR_OREFLAG(&s_serial_port.uart);
    HAL_NVIC_ClearPendingIRQ(USART1_IRQn);
    s_serial_port.rx_queue = rx_queue;
    status = HAL_UART_Receive_IT(&s_serial_port.uart, &s_serial_port.rx_byte, 1U);
    if (status == HAL_OK)
    {
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    }
    else
    {
        s_serial_port.rx_queue = NULL;
    }
    return status;
}

HAL_StatusTypeDef SerialPort_Service(void)
{
    HAL_StatusTypeDef status;

    if ((s_serial_port.initialized == 0U) || (s_serial_port.rx_queue == NULL))
    {
        return HAL_ERROR;
    }
    if (s_serial_port.rx_recovery_required == 0U)
    {
        return HAL_OK;
    }

    /* Pause only USART1 while the task repairs the HAL receive state.
     * DHT11 capture and the FreeRTOS tick remain enabled. No DMA is used. */
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    status = HAL_UART_AbortReceive(&s_serial_port.uart);
    if (status == HAL_OK)
    {
        __HAL_UART_CLEAR_OREFLAG(&s_serial_port.uart);
        status = HAL_UART_Receive_IT(&s_serial_port.uart, &s_serial_port.rx_byte, 1U);
    }
    if (status == HAL_OK)
    {
        s_serial_port.rx_recovery_required = 0U;
    }
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    return status;
}

HAL_StatusTypeDef SerialPort_Write(const uint8_t *data, uint16_t length)
{
    HAL_StatusTypeDef status;

    if ((s_serial_port.initialized == 0U) ||
        ((data == NULL) && (length != 0U)) || (length > SERIAL_TX_MAX_BYTES))
    {
        return HAL_ERROR;
    }
    if (length == 0U)
    {
        return HAL_OK;
    }

    status = HAL_UART_Transmit(&s_serial_port.uart, (uint8_t *)data,
                               length, SERIAL_TX_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        ++s_serial_port.tx_error_count;
    }
    return status;
}

HAL_StatusTypeDef SerialPort_WriteString(const char *text)
{
    uint16_t length = 0U;

    if (text == NULL)
    {
        return HAL_ERROR;
    }
    /* Bound the search and transfer so a long string cannot stall this task. */
    while ((length < SERIAL_TX_MAX_BYTES) && (text[length] != '\0'))
    {
        ++length;
    }
    if (text[length] != '\0')
    {
        return HAL_ERROR;
    }
    return SerialPort_Write((const uint8_t *)text, length);
}

void SerialPort_GetStats(SerialPort_Stats *stats)
{
    if (stats == NULL)
    {
        return;
    }

    /* USART1 priority 6 is masked here; high-priority DHT11 EXTI is not. */
    taskENTER_CRITICAL();
    stats->rx_received_count = s_serial_port.rx_received_count;
    stats->rx_dropped_count = s_serial_port.rx_dropped_count;
    stats->rx_error_count = s_serial_port.rx_error_count;
    stats->tx_error_count = s_serial_port.tx_error_count;
    stats->rx_recovery_required = s_serial_port.rx_recovery_required;
    taskEXIT_CRITICAL();
}

void SerialPort_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_serial_port.uart);
}

/* HAL callbacks: interrupt context ---------------------------------------- */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart)
{
    BaseType_t task_woken = pdFALSE;

    if ((uart != &s_serial_port.uart) || (s_serial_port.rx_queue == NULL))
    {
        return;
    }

    ++s_serial_port.rx_received_count;
    if (xQueueSendFromISR(s_serial_port.rx_queue, &s_serial_port.rx_byte,
                          &task_woken) != pdPASS)
    {
        ++s_serial_port.rx_dropped_count;
    }

    if ((s_serial_port.rx_recovery_required == 0U) &&
        (HAL_UART_Receive_IT(uart, &s_serial_port.rx_byte, 1U) != HAL_OK))
    {
        ++s_serial_port.rx_error_count;
        s_serial_port.rx_recovery_required = 1U;
    }
    portYIELD_FROM_ISR(task_woken);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart)
{
    if (uart == &s_serial_port.uart)
    {
        ++s_serial_port.rx_error_count;
        s_serial_port.rx_recovery_required = 1U;
    }
}
