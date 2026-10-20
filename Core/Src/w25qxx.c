#include "w25qxx.h"
#include "gpio.h"
#include "project_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define CMD_READ_ID       0x9FU
#define CMD_READ_STATUS   0x05U
#define CMD_WRITE_ENABLE  0x06U
#define CMD_READ          0x03U
#define CMD_PROGRAM       0x02U
#define CMD_ERASE_SECTOR  0x20U
#define STATUS_BUSY       0x01U
#define STATUS_WEL        0x02U

static SPI_HandleTypeDef s_spi;
static W25Q_Info s_info;
static W25Q_Diagnostic s_diagnostic;
static uint32_t s_operation_address;
static HAL_StatusTypeDef s_last_spi_result;

static void select_chip(uint8_t selected)
{
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin,
                      (selected != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void save_failure(W25Q_IoStage stage, HAL_StatusTypeDef result, uint32_t start)
{
    s_diagnostic.stage = stage;
    s_diagnostic.address = s_operation_address;
    s_diagnostic.hal_status = (uint32_t)result;
    s_diagnostic.hal_error = s_spi.ErrorCode;
    s_diagnostic.elapsed_ms = HAL_GetTick() - start;
}

static uint8_t transmit(const uint8_t *data, uint16_t length)
{
    uint32_t start = HAL_GetTick();
    s_last_spi_result = HAL_SPI_Transmit(&s_spi, (uint8_t *)data, length,
                                         FLASH_SPI_TRANSFER_TIMEOUT_MS);
    if (s_last_spi_result == HAL_OK) { return 1U; }
    save_failure(W25Q_IO_TRANSMIT, s_last_spi_result, start);
    return 0U;
}

static uint8_t exchange(uint8_t *tx, uint8_t *rx, uint16_t length, W25Q_IoStage stage)
{
    uint32_t start = HAL_GetTick();
    s_last_spi_result = HAL_SPI_TransmitReceive(&s_spi, tx, rx, length,
                                                FLASH_SPI_TRANSFER_TIMEOUT_MS);
    if (s_last_spi_result == HAL_OK) { return 1U; }
    save_failure(stage, s_last_spi_result, start);
    return 0U;
}

static uint8_t read_status(uint8_t *status)
{
    uint8_t tx[2] = {CMD_READ_STATUS, 0xFFU};
    uint8_t rx[2] = {0U};
    uint8_t result;

    select_chip(1U);
    result = exchange(tx, rx, 2U, W25Q_IO_STATUS);
    select_chip(0U);
    *status = rx[1];
    return result;
}

static uint8_t wait_ready(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t status;

    do
    {
        if (read_status(&status) == 0U) { return 0U; }
        if ((status & STATUS_BUSY) == 0U) { return 1U; }
        vTaskDelay(pdMS_TO_TICKS(1U));
    } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);
    save_failure(W25Q_IO_WAIT_READY, HAL_OK, start);
    return 0U;
}

static uint8_t write_enable(void)
{
    uint8_t command = CMD_WRITE_ENABLE;
    uint8_t status;
    uint8_t result;

    if (wait_ready(FLASH_ERASE_TIMEOUT_MS) == 0U) { return 0U; }
    select_chip(1U);
    result = transmit(&command, 1U);
    select_chip(0U);
    return ((result != 0U) && (read_status(&status) != 0U) &&
            ((status & (STATUS_WEL | STATUS_BUSY)) == STATUS_WEL)) ? 1U : 0U;
}

static uint8_t address_command(uint8_t command, uint32_t address)
{
    uint8_t data[4];
    data[0] = command;
    data[1] = (uint8_t)(address >> 16U);
    data[2] = (uint8_t)(address >> 8U);
    data[3] = (uint8_t)address;
    return transmit(data, sizeof(data));
}

static uint8_t range_valid(uint32_t address, uint32_t length)
{
    return ((s_info.ready != 0U) && (address < s_info.capacity_bytes) &&
            (length > 0U) && (length <= s_info.capacity_bytes - address)) ? 1U : 0U;
}

uint8_t W25Q_Init(void)
{
    uint8_t tx[4] = {CMD_READ_ID, 0xFFU, 0xFFU, 0xFFU};
    uint8_t rx[4] = {0U};
    uint8_t release = 0xABU;
    uint8_t result;

    (void)memset(&s_info, 0, sizeof(s_info));
    (void)memset(&s_diagnostic, 0, sizeof(s_diagnostic));
    s_operation_address = 0U;
    s_last_spi_result = HAL_OK;
    __HAL_RCC_SPI1_CLK_ENABLE();
    s_spi.Instance = SPI1;
    s_spi.Init.Mode = SPI_MODE_MASTER;
    s_spi.Init.Direction = SPI_DIRECTION_2LINES;
    s_spi.Init.DataSize = SPI_DATASIZE_8BIT;
    s_spi.Init.CLKPolarity = SPI_POLARITY_LOW;
    s_spi.Init.CLKPhase = SPI_PHASE_1EDGE;
    s_spi.Init.NSS = SPI_NSS_SOFT;
    s_spi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
    s_spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    s_spi.Init.TIMode = SPI_TIMODE_DISABLE;
    s_spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    s_spi.Init.CRCPolynomial = 7U;
    if (HAL_SPI_Init(&s_spi) != HAL_OK) { return 0U; }
    select_chip(0U);
    vTaskDelay(pdMS_TO_TICKS(FLASH_POWER_UP_DELAY_MS));
    select_chip(1U);
    result = transmit(&release, 1U);
    select_chip(0U);
    if (result == 0U) { return 0U; }
    vTaskDelay(pdMS_TO_TICKS(1U));
    if (wait_ready(FLASH_ERASE_TIMEOUT_MS) == 0U) { return 0U; }
    select_chip(1U);
    result = exchange(tx, rx, 4U, W25Q_IO_IDENTIFY);
    select_chip(0U);
    s_info.jedec_id = ((uint32_t)rx[1] << 16U) | ((uint32_t)rx[2] << 8U) | rx[3];
    if ((result == 0U) || (rx[1] != 0xEFU) || (rx[2] != 0x40U) ||
        (rx[3] < 0x14U) || (rx[3] > 0x18U)) { return 0U; }
    s_info.capacity_bytes = 1UL << rx[3];
    s_info.ready = 1U;
    return 1U;
}

void W25Q_GetInfo(W25Q_Info *info)
{
    if (info != NULL) { *info = s_info; }
}

void W25Q_GetDiagnostic(W25Q_Diagnostic *diagnostic)
{
    if (diagnostic != NULL) { *diagnostic = s_diagnostic; }
}

static uint8_t recover_spi(void)
{
    uint32_t start = HAL_GetTick();
    HAL_StatusTypeDef result;

    /* CS high aborts the read command. Reset only the MCU SPI peripheral,
     * leaving W25Q contents and its write/erase state untouched. */
    select_chip(0U);
    result = HAL_SPI_DeInit(&s_spi);
    if (result == HAL_OK)
    {
        __HAL_RCC_SPI1_FORCE_RESET();
        __HAL_RCC_SPI1_RELEASE_RESET();
        result = HAL_SPI_Init(&s_spi);
    }
    if (result == HAL_OK) { return 1U; }
    save_failure(W25Q_IO_RECOVERY, result, start);
    return 0U;
}

static uint8_t read_once(uint32_t address, void *data, uint32_t length)
{
    uint8_t tx[32];
    uint8_t *output = (uint8_t *)data;
    uint16_t chunk;
    uint8_t result = 1U;

    if (wait_ready(FLASH_ERASE_TIMEOUT_MS) == 0U) { return 0U; }
    (void)memset(tx, 0xFF, sizeof(tx));
    select_chip(1U);
    result = address_command(CMD_READ, address);
    while ((result != 0U) && (length > 0U))
    {
        chunk = (length > sizeof(tx)) ? sizeof(tx) : (uint16_t)length;
        result = exchange(tx, output, chunk, W25Q_IO_READ_DATA);
        output += chunk;
        length -= chunk;
    }
    select_chip(0U);
    return result;
}

uint8_t W25Q_Read(uint32_t address, void *data, uint32_t length)
{
    uint32_t attempt;

    if ((data == NULL) || (range_valid(address, length) == 0U)) { return 0U; }
    s_operation_address = address;
    for (attempt = 0U; attempt < FLASH_READ_MAX_ATTEMPTS; ++attempt)
    {
        if (read_once(address, data, length) != 0U) { return 1U; }
        /* Retry only transport failures, not a persistently BUSY Flash.
         * Restart the complete read so partial receive buffers are replaced. */
        if ((s_last_spi_result == HAL_OK) || (attempt + 1U >= FLASH_READ_MAX_ATTEMPTS)) { break; }
        vTaskDelay(pdMS_TO_TICKS(FLASH_READ_RETRY_DELAY_MS));
        if (recover_spi() == 0U) { break; }
        ++s_diagnostic.read_retries;
    }
    return 0U;
}

uint8_t W25Q_Program(uint32_t address, const void *data, uint32_t length)
{
    const uint8_t *input = (const uint8_t *)data;
    uint32_t chunk;
    uint8_t result;

    if ((data == NULL) || (range_valid(address, length) == 0U)) { return 0U; }
    s_operation_address = address;
    while (length > 0U)
    {
        chunk = FLASH_PAGE_BYTES - address % FLASH_PAGE_BYTES;
        if (chunk > length) { chunk = length; }
        if (write_enable() == 0U) { return 0U; }
        select_chip(1U);
        result = address_command(CMD_PROGRAM, address);
        if (result != 0U) { result = transmit(input, (uint16_t)chunk); }
        select_chip(0U);
        if ((result == 0U) || (wait_ready(FLASH_PROGRAM_TIMEOUT_MS) == 0U)) { return 0U; }
        input += chunk;
        address += chunk;
        length -= chunk;
    }
    return 1U;
}

uint8_t W25Q_EraseSector(uint32_t address)
{
    uint8_t result;
    s_operation_address = address;
    if ((range_valid(address, FLASH_SECTOR_BYTES) == 0U) ||
        ((address % FLASH_SECTOR_BYTES) != 0U) || (write_enable() == 0U)) { return 0U; }
    select_chip(1U);
    result = address_command(CMD_ERASE_SECTOR, address);
    select_chip(0U);
    return ((result != 0U) && (wait_ready(FLASH_ERASE_TIMEOUT_MS) != 0U)) ? 1U : 0U;
}
