#include "dht11.h"
#include "board_timing.h"
#include "project_config.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

/* Private protocol constants --------------------------------------------- */
#define DAT_PIN GPIO_PIN_10
#define DHT11_HOST_START_LOW_MS 20U
#define DHT11_FRAME_TIMEOUT_MS  12U

/* Private types ----------------------------------------------------------- */
typedef struct
{
    DHT11_Decoder decoder;             /* Its capture fields are volatile. */
    volatile uint32_t last_edge_cycles; /* Shared between task and EXTI ISR. */
    uint32_t last_attempt_ms;          /* Only the acquisition task uses it. */
    uint8_t initialized;
} DHT11_Context;

/* Private variables ------------------------------------------------------- */
static DHT11_Context s_dht11;

/* Private functions ------------------------------------------------------- */

static void input_idle(void)
{
    GPIO_InitTypeDef gpio = {0};
    EXTI->IMR &= ~(uint32_t)DAT_PIN;
    __HAL_GPIO_EXTI_CLEAR_IT(DAT_PIN);
    gpio.Pin = DAT_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &gpio);
}

/* Public functions -------------------------------------------------------- */
uint8_t DHT11_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();
    s_dht11.initialized = BoardTiming_Init();
    input_idle();
    /* Higher urgency than the kernel's BASEPRI mask. ISR only captures edges. */
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 4U, 0U);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
    s_dht11.last_attempt_ms = HAL_GetTick();
    return s_dht11.initialized;
}

DHT11_Status DHT11_Read(DHT11_Reading *reading)
{
    GPIO_InitTypeDef gpio = {0};
    uint32_t start;
    uint32_t primask;
    DHT11_DecodeState state;
    DHT11_FrameResult result;

    if ((s_dht11.initialized == 0U) || (reading == 0)) { return DHT11_TIMING_ERROR; }
    start = HAL_GetTick();
    if ((uint32_t)(start - s_dht11.last_attempt_ms) < DHT11_MIN_READ_INTERVAL_MS) { return DHT11_WAIT; }
    s_dht11.last_attempt_ms = start;
    if ((GPIOB->IDR & DAT_PIN) == 0U) { return DHT11_BUS_LOW; }

    EXTI->IMR &= ~(uint32_t)DAT_PIN;
    /* Only pull low; never drive the data line high with a push-pull output. */
    HAL_GPIO_WritePin(GPIOB, DAT_PIN, GPIO_PIN_RESET);
    gpio.Pin = DAT_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    /* >=18 ms host start with the configured 1 kHz RTOS tick. */
    vTaskDelay(pdMS_TO_TICKS(DHT11_HOST_START_LOW_MS));

    DHT11_DecoderReset(&s_dht11.decoder);
    /* This short critical section covers only the GPIO/EXTI handover.
     * The 4-5 ms reply is captured with interrupts ENABLED. */
    primask = __get_PRIMASK();
    __disable_irq();
    __HAL_GPIO_EXTI_CLEAR_IT(DAT_PIN);
    s_dht11.last_edge_cycles = BoardTiming_Cycles();
    gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &gpio);  /* Input pull-up releases DAT and arms EXTI10. */
    __set_PRIMASK(primask);

    start = HAL_GetTick();
    do {
        state = s_dht11.decoder.state;
        if ((state == DHT_DEC_COMPLETE) || (state == DHT_DEC_INVALID)) { break; }
        vTaskDelay(pdMS_TO_TICKS(1U));
    } while ((uint32_t)(HAL_GetTick() - start) < DHT11_FRAME_TIMEOUT_MS);

    /* Mask the capture line before accessing the completed buffer in task mode. */
    primask = __get_PRIMASK();
    __disable_irq();
    EXTI->IMR &= ~(uint32_t)DAT_PIN;
    state = s_dht11.decoder.state;
    __HAL_GPIO_EXTI_CLEAR_IT(DAT_PIN);
    __set_PRIMASK(primask);
    input_idle();

    if (state == DHT_DEC_INVALID) { return DHT11_PROTOCOL_ERROR; }
    if (state != DHT_DEC_COMPLETE) { return DHT11_TIMEOUT; }
    result = DHT11_DecoderResult(&s_dht11.decoder, reading);
    if (result == DHT_FRAME_CHECKSUM) { return DHT11_CHECKSUM_ERROR; }
    return (result == DHT_FRAME_OK) ? DHT11_OK : DHT11_PROTOCOL_ERROR;
}

void DHT11_EXTI_IRQHandler(void)
{
    uint32_t now;
    uint32_t elapsed;
    uint8_t level;
    if (((EXTI->PR & DAT_PIN) == 0U) || ((EXTI->IMR & DAT_PIN) == 0U)) { return; }
    now = BoardTiming_Cycles();
    level = (uint8_t)((GPIOB->IDR & DAT_PIN) != 0U);
    __HAL_GPIO_EXTI_CLEAR_IT(DAT_PIN);
    elapsed = BoardTiming_CyclesToUs(now - s_dht11.last_edge_cycles);
    s_dht11.last_edge_cycles = now;
    DHT11_DecoderEdge(&s_dht11.decoder, level, elapsed);
    if ((s_dht11.decoder.state == DHT_DEC_COMPLETE) || (s_dht11.decoder.state == DHT_DEC_INVALID)) {
        EXTI->IMR &= ~(uint32_t)DAT_PIN;
    }
}

const char *DHT11_StatusText(DHT11_Status status)
{
    switch (status) {
    case DHT11_OK: return "OK";
    case DHT11_WAIT: return "WAIT";
    case DHT11_TIMEOUT: return "TIMEOUT";
    case DHT11_CHECKSUM_ERROR: return "CHECKSUM";
    case DHT11_PROTOCOL_ERROR: return "PROTOCOL";
    case DHT11_BUS_LOW: return "BUS LOW";
    default: return "TIMING";
    }
}
