#include "soft_i2c.h"
#include "board_timing.h"
#include "main.h"

#define SCL_PIN GPIO_PIN_6
#define SDA_PIN GPIO_PIN_7
#define HALF_CLOCK_US 5U
#define STRETCH_TIMEOUT_US 1000U

static void scl_low(void) { GPIOB->BRR = SCL_PIN; }
static void sda_low(void) { GPIOB->BRR = SDA_PIN; }
static void sda_release(void) { GPIOB->BSRR = SDA_PIN; }
static uint8_t sda_read(void) { return (uint8_t)((GPIOB->IDR & SDA_PIN) != 0U); }

static uint8_t scl_release(void)
{
    uint32_t start;
    GPIOB->BSRR = SCL_PIN;
    start = BoardTiming_Cycles();
    while ((GPIOB->IDR & SCL_PIN) == 0U) {
        if (BoardTiming_ElapsedUs(start) >= STRETCH_TIMEOUT_US) {
            return 0U;
        }
    }
    return 1U;
}

static void stop_condition(void)
{
    scl_low();
    sda_low();
    BoardTiming_DelayUs(HALF_CLOCK_US);
    (void)scl_release();
    BoardTiming_DelayUs(HALF_CLOCK_US);
    sda_release();
    BoardTiming_DelayUs(HALF_CLOCK_US);
}

static uint8_t start_condition(void)
{
    sda_release();
    if (scl_release() == 0U) {
        return 0U;
    }
    BoardTiming_DelayUs(HALF_CLOCK_US);
    if (sda_read() == 0U) {
        return 0U;
    }
    sda_low();
    BoardTiming_DelayUs(HALF_CLOCK_US);
    scl_low();
    return 1U;
}

static uint8_t write_byte(uint8_t value)
{
    uint8_t i;
    uint8_t ack;
    for (i = 0U; i < 8U; ++i) {
        if ((value & 0x80U) != 0U) {
            sda_release();
        } else {
            sda_low();
        }
        BoardTiming_DelayUs(HALF_CLOCK_US);
        if (scl_release() == 0U) {
            return 0U;
        }
        BoardTiming_DelayUs(HALF_CLOCK_US);
        scl_low();
        value <<= 1;
    }
    /* Release SDA for the slave's ninth-clock ACK; read IDR while open-drain. */
    sda_release();
    BoardTiming_DelayUs(HALF_CLOCK_US);
    if (scl_release() == 0U) {
        return 0U;
    }
    BoardTiming_DelayUs(HALF_CLOCK_US);
    ack = (uint8_t)(sda_read() == 0U);
    scl_low();
    BoardTiming_DelayUs(HALF_CLOCK_US);
    return ack;
}

uint8_t SoftI2C_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t i;
    if (BoardTiming_Init() == 0U) {
        return 0U;
    }
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, SCL_PIN | SDA_PIN, GPIO_PIN_SET);
    gpio.Pin = SCL_PIN | SDA_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    sda_release();
    if (scl_release() == 0U) {
        return 0U;
    }
    /* Recover a slave left partway through a byte by an MCU reset. */
    for (i = 0U; (i < 9U) && (sda_read() == 0U); ++i) {
        scl_low();
        BoardTiming_DelayUs(HALF_CLOCK_US);
        if (scl_release() == 0U) {
            return 0U;
        }
        BoardTiming_DelayUs(HALF_CLOCK_US);
    }
    stop_condition();
    return (uint8_t)(sda_read() != 0U);
}

uint8_t SoftI2C_Probe(uint8_t address_7bit)
{
    uint8_t ok = start_condition();
    if (ok != 0U) {
        ok = write_byte((uint8_t)(address_7bit << 1));
    }
    stop_condition();
    return ok;
}

uint8_t SoftI2C_Write(uint8_t address_7bit, uint8_t control,
                      const uint8_t *data, uint16_t length)
{
    uint16_t i;
    uint8_t ok;
    if ((data == 0) && (length != 0U)) {
        return 0U;
    }
    ok = start_condition();
    if (ok != 0U) { ok = write_byte((uint8_t)(address_7bit << 1)); }
    if (ok != 0U) { ok = write_byte(control); }
    for (i = 0U; (i < length) && (ok != 0U); ++i) {
        ok = write_byte(data[i]);
    }
    stop_condition();
    return ok;
}
