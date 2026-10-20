#include "serial_port.h"
#include "serial_report.h"
#include "project_config.h"
#include <string.h>

/* Test the real serial driver at its HAL and FreeRTOS boundaries. */
static UART_HandleTypeDef *bound_uart;
static uint8_t *pending_rx;
static HAL_StatusTypeDef next_init_status;
static HAL_StatusTypeDef next_rx_status;
static HAL_StatusTypeDef next_abort_status;
static HAL_StatusTypeDef next_tx_status;
static uint32_t init_calls, rx_calls, abort_calls, tx_calls, irq_calls;
static uint32_t irq_enables, irq_disables, clear_calls, yields;
static uint32_t critical_depth, boundary_errors;
static uint16_t sent_length;
static uint32_t sent_timeout;
static uint8_t sent_data[SERIAL_TX_MAX_BYTES];
static TestByteQueue rx_queue;
static uint32_t failures;

#define CHECK(group, condition) \
    do { if (!(condition)) { failures |= (1UL << (group)); } } while (0)

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *uart)
{
    HAL_StatusTypeDef status = next_init_status;
    ++init_calls;
    bound_uart = uart;
    next_init_status = HAL_OK;
    return status;
}

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *uart, uint8_t *data, uint16_t length)
{
    HAL_StatusTypeDef status = next_rx_status;
    ++rx_calls;
    next_rx_status = HAL_OK;
    if ((uart != bound_uart) || (length != 1U)) { ++boundary_errors; }
    if (status == HAL_OK) { pending_rx = data; }
    return status;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *uart)
{
    HAL_StatusTypeDef status = next_abort_status;
    ++abort_calls;
    next_abort_status = HAL_OK;
    if (uart != bound_uart) { ++boundary_errors; }
    return status;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *uart, uint8_t *data,
                                   uint16_t length, uint32_t timeout)
{
    HAL_StatusTypeDef status = next_tx_status;
    ++tx_calls;
    next_tx_status = HAL_OK;
    if (uart != bound_uart) { ++boundary_errors; }
    sent_length = length;
    sent_timeout = timeout;
    (void)memcpy(sent_data, data, length);
    return status;
}

void HAL_UART_IRQHandler(UART_HandleTypeDef *uart)
{
    ++irq_calls;
    if (uart != bound_uart) { ++boundary_errors; }
}
void test_clear_error(UART_HandleTypeDef *uart)
{
    ++clear_calls;
    if (uart != bound_uart) { ++boundary_errors; }
}
void HAL_NVIC_ClearPendingIRQ(int irq) { if (irq != USART1_IRQn) { ++boundary_errors; } }
void HAL_NVIC_EnableIRQ(int irq)
{
    ++irq_enables;
    if (irq != USART1_IRQn) { ++boundary_errors; }
}
void HAL_NVIC_DisableIRQ(int irq)
{
    ++irq_disables;
    if (irq != USART1_IRQn) { ++boundary_errors; }
}
void test_yield_from_isr(BaseType_t wake) { if (wake != pdFALSE) { ++yields; } }
void test_enter_critical(void) { ++critical_depth; }
void test_exit_critical(void) { --critical_depth; }

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item, BaseType_t *wake)
{
    if (queue != &rx_queue) { ++boundary_errors; return pdFAIL; }
    if (queue->count >= sizeof(queue->bytes)) { return pdFAIL; }
    if (queue->count == 0U) { *wake = pdTRUE; }
    queue->bytes[queue->count++] = *(const uint8_t *)item;
    return pdPASS;
}

static void receive_byte(uint8_t value)
{
    *pending_rx = value;
    HAL_UART_RxCpltCallback(bound_uart);
}

uint32_t RunSerialPortTests(void)
{
    SerialPort_Stats stats;
    UART_HandleTypeDef other_uart = {0};
    char text[SERIAL_TX_MAX_BYTES + 2U];
    uint32_t previous;
    EnvAlarm_Sample sample;
    const char *expected;

    CHECK(0, SerialPort_WriteString("X") == HAL_ERROR);
    CHECK(0, SerialPort_StartReceive(&rx_queue) == HAL_ERROR);
    CHECK(0, SerialPort_Service() == HAL_ERROR);
    next_init_status = HAL_ERROR;
    CHECK(0, SerialPort_Init() == HAL_ERROR);
    CHECK(0, SerialPort_Init() == HAL_OK);
    CHECK(0, bound_uart->Instance == USART1);
    CHECK(0, (bound_uart->Init.BaudRate == SERIAL_BAUD_RATE) &&
             (bound_uart->Init.WordLength == UART_WORDLENGTH_8B) &&
             (bound_uart->Init.StopBits == UART_STOPBITS_1) &&
             (bound_uart->Init.Parity == UART_PARITY_NONE) &&
             (bound_uart->Init.Mode == UART_MODE_TX_RX) &&
             (bound_uart->Init.HwFlowCtl == UART_HWCONTROL_NONE));
    CHECK(0, (SerialPort_Init() == HAL_OK) && (init_calls == 2U));
    CHECK(0, irq_enables == 0U);

    CHECK(1, SerialPort_WriteString("SYSTEM READY\r\n") == HAL_OK);
    CHECK(1, (sent_length == 14U) && (sent_timeout == SERIAL_TX_TIMEOUT_MS));
    CHECK(1, memcmp(sent_data, "SYSTEM READY\r\n", 14U) == 0);
    previous = tx_calls;
    CHECK(1, SerialPort_WriteString(NULL) == HAL_ERROR);
    CHECK(1, SerialPort_Write(NULL, 1U) == HAL_ERROR);
    CHECK(1, SerialPort_Write(NULL, 0U) == HAL_OK);
    CHECK(1, SerialPort_WriteString("") == HAL_OK);
    (void)memset(text, 'A', sizeof(text));
    text[SERIAL_TX_MAX_BYTES + 1U] = '\0';
    CHECK(1, SerialPort_WriteString(text) == HAL_ERROR);
    CHECK(1, tx_calls == previous);
    text[SERIAL_TX_MAX_BYTES] = '\0';
    CHECK(1, SerialPort_WriteString(text) == HAL_OK);
    CHECK(1, sent_length == SERIAL_TX_MAX_BYTES);
    next_tx_status = HAL_TIMEOUT;
    CHECK(1, SerialPort_WriteString("X") == HAL_TIMEOUT);
    next_tx_status = HAL_BUSY;
    CHECK(1, SerialPort_WriteString("X") == HAL_BUSY);
    SerialPort_GetStats(&stats);
    CHECK(1, stats.tx_error_count == 2U);

    CHECK(2, SerialPort_StartReceive(NULL) == HAL_ERROR);
    next_rx_status = HAL_ERROR;
    CHECK(2, SerialPort_StartReceive(&rx_queue) == HAL_ERROR);
    CHECK(2, irq_enables == 0U);
    CHECK(2, SerialPort_StartReceive(&rx_queue) == HAL_OK);
    CHECK(2, irq_enables == 1U);
    CHECK(2, SerialPort_StartReceive(&rx_queue) == HAL_BUSY);
    receive_byte('A');
    receive_byte('B');
    receive_byte('C');
    CHECK(2, (rx_queue.count == 2U) && (rx_queue.bytes[0] == 'A') && (rx_queue.bytes[1] == 'B'));
    SerialPort_GetStats(&stats);
    CHECK(2, (stats.rx_received_count == 3U) && (stats.rx_dropped_count == 1U));
    CHECK(2, yields != 0U);
    CHECK(2, stats.rx_recovery_required == 0U);

    rx_queue.count = 0U;
    next_rx_status = HAL_ERROR;
    receive_byte('D');
    SerialPort_GetStats(&stats);
    CHECK(3, (stats.rx_error_count == 1U) && (stats.rx_recovery_required == 1U));
    next_abort_status = HAL_ERROR;
    CHECK(3, SerialPort_Service() == HAL_ERROR);
    next_rx_status = HAL_BUSY;
    CHECK(3, SerialPort_Service() == HAL_BUSY);
    CHECK(3, SerialPort_Service() == HAL_OK);
    SerialPort_GetStats(&stats);
    CHECK(3, stats.rx_recovery_required == 0U);
    CHECK(3, (abort_calls == 3U) && (irq_disables == 3U));
    previous = rx_calls;
    CHECK(3, SerialPort_Service() == HAL_OK);
    CHECK(3, rx_calls == previous);
    receive_byte('E');
    CHECK(3, (rx_queue.count == 2U) && (rx_queue.bytes[1] == 'E'));

    HAL_UART_ErrorCallback(bound_uart);
    SerialPort_GetStats(&stats);
    CHECK(4, (stats.rx_error_count == 2U) && (stats.rx_recovery_required == 1U));
    CHECK(4, SerialPort_Service() == HAL_OK);
    SerialPort_GetStats(&stats);
    CHECK(4, stats.rx_recovery_required == 0U);
    previous = stats.rx_received_count;
    HAL_UART_RxCpltCallback(&other_uart);
    HAL_UART_ErrorCallback(&other_uart);
    SerialPort_GetStats(&stats);
    CHECK(4, (stats.rx_received_count == previous) && (stats.rx_error_count == 2U));

    SerialPort_IRQHandler();
    SerialPort_GetStats(NULL);
    CHECK(5, (irq_calls == 1U) && (critical_depth == 0U));
    CHECK(5, (clear_calls >= 3U) && (boundary_errors == 0U));

    sample.temperature_x10 = 256U;
    sample.humidity_x10 = 610U;
    sample.valid = 1U;
    expected = "ENV TEMP=25.6C HUM=61.0% SENSOR=OK\r\n";
    CHECK(6, SerialReport_SendSample(&sample) == HAL_OK);
    CHECK(6, sent_length == strlen(expected));
    CHECK(6, memcmp(sent_data, expected, sent_length) == 0);

    /* The payload still holds last-good numbers, but they must not be sent. */
    sample.valid = 0U;
    expected = "ENV TEMP=--.-C HUM=--.-% SENSOR=ERROR\r\n";
    CHECK(7, SerialReport_SendSample(&sample) == HAL_OK);
    CHECK(7, sent_length == strlen(expected));
    CHECK(7, memcmp(sent_data, expected, sent_length) == 0);

    previous = tx_calls;
    CHECK(8, SerialReport_SendSample(NULL) == HAL_ERROR);
    CHECK(8, tx_calls == previous);
    sample.valid = 1U;
    sample.temperature_x10 = 0U;
    sample.humidity_x10 = 1U;
    expected = "ENV TEMP=0.0C HUM=0.1% SENSOR=OK\r\n";
    CHECK(8, SerialReport_SendSample(&sample) == HAL_OK);
    CHECK(8, sent_length == strlen(expected));
    CHECK(8, memcmp(sent_data, expected, sent_length) == 0);
    sample.temperature_x10 = 65535U;
    sample.humidity_x10 = 65535U;
    expected = "ENV TEMP=6553.5C HUM=6553.5% SENSOR=OK\r\n";
    CHECK(8, SerialReport_SendSample(&sample) == HAL_OK);
    CHECK(8, sent_length == strlen(expected));
    CHECK(8, memcmp(sent_data, expected, sent_length) == 0);

    next_tx_status = HAL_TIMEOUT;
    CHECK(9, SerialReport_SendSample(&sample) == HAL_TIMEOUT);
    sample.temperature_x10 = 300U;
    sample.humidity_x10 = 700U;
    expected = "ENV TEMP=30.0C HUM=70.0% SENSOR=OK\r\n";
    CHECK(9, SerialReport_SendSample(&sample) == HAL_OK);
    CHECK(9, sent_length == strlen(expected));
    CHECK(9, memcmp(sent_data, expected, sent_length) == 0);
    SerialPort_GetStats(&stats);
    CHECK(9, stats.tx_error_count == 3U);
    return failures;
}
