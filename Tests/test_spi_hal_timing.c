#include "stm32f1xx_hal.h"
#include "project_config.h"
#include <string.h>

/* Compile the unmodified vendor HAL SPI driver. The register model keeps
 * TXE/RXNE asserted and BSY clear, while HAL_GetTick injects elapsed time
 * representing another task running. This tests HAL's actual timeout path,
 * not OLED hardware or a complete FreeRTOS scheduling simulation. */
static uint32_t ticks;
static uint32_t tick_calls;
static uint8_t progressing;
static uint32_t dma_calls;
uint32_t SystemCoreClock = 72000000U;

/* Link-only DMA dependencies from the vendor SPI translation unit.
 * The polling paths tested below must never call them. */
HAL_StatusTypeDef HAL_DMA_Abort(DMA_HandleTypeDef *dma)
{ (void)dma; ++dma_calls; return HAL_ERROR; }
HAL_StatusTypeDef HAL_DMA_Abort_IT(DMA_HandleTypeDef *dma)
{ (void)dma; ++dma_calls; return HAL_ERROR; }
HAL_StatusTypeDef HAL_DMA_Start_IT(DMA_HandleTypeDef *dma, uint32_t source,
                                  uint32_t destination, uint32_t length)
{ (void)dma; (void)source; (void)destination; (void)length; ++dma_calls; return HAL_ERROR; }

uint32_t HAL_GetTick(void)
{
    ++tick_calls;
    if (tick_calls == 2U) { ticks += 180U; }
    if (progressing != 0U) { ++ticks; }
    return ticks;
}

uint32_t RunSpiHalTimingTests(void)
{
    SPI_HandleTypeDef spi;
    uint8_t tx[32];
    uint8_t rx[32];
    uint32_t failures = 0U;
    uint32_t index;
    HAL_StatusTypeDef result;

    (void)memset(&spi, 0, sizeof(spi));
    (void)memset(tx, 0xFF, sizeof(tx));
    spi.Instance = SPI1;
    spi.Init.Mode = SPI_MODE_MASTER;
    spi.Init.Direction = SPI_DIRECTION_2LINES;
    spi.Init.DataSize = SPI_DATASIZE_8BIT;
    spi.State = HAL_SPI_STATE_READY;
    SPI1->SR = SPI_SR_TXE | SPI_SR_RXNE;

    result = HAL_SPI_TransmitReceive(&spi, tx, rx, sizeof(tx), 20U);
    if ((result != HAL_TIMEOUT) || (ticks != 180U)) { failures |= 1U; }

    ticks = tick_calls = 0U;
    result = HAL_SPI_TransmitReceive(&spi, tx, rx, sizeof(tx), FLASH_SPI_TRANSFER_TIMEOUT_MS);
    if ((result != HAL_OK) || (ticks != 180U)) { failures |= 2U; }
    for (index = 0U; index < sizeof(rx); ++index)
    { if (rx[index] != 0xFFU) { failures |= 2U; } }

    ticks = 0xFFFFFF80UL;
    tick_calls = 0U;
    result = HAL_SPI_TransmitReceive(&spi, tx, rx, sizeof(tx), FLASH_SPI_TRANSFER_TIMEOUT_MS);
    if (result != HAL_OK) { failures |= 4U; }

    ticks = tick_calls = 0U;
    progressing = 1U;
    SPI1->SR = 0U; /* Hardware never asserts TXE/RXNE. Timeout must stay bounded. */
    result = HAL_SPI_TransmitReceive(&spi, tx, rx, sizeof(tx), FLASH_SPI_TRANSFER_TIMEOUT_MS);
    if ((result != HAL_TIMEOUT) || (ticks > FLASH_SPI_TRANSFER_TIMEOUT_MS + 2U) ||
        (dma_calls != 0U)) { failures |= 8U; }
    return failures;
}
