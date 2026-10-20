#ifndef W25QXX_H
#define W25QXX_H

#include <stdint.h>

typedef struct
{
    uint32_t jedec_id;
    uint32_t capacity_bytes;
    uint8_t ready;
} W25Q_Info;

typedef enum
{
    W25Q_IO_NONE = 0,
    W25Q_IO_STATUS,
    W25Q_IO_TRANSMIT,
    W25Q_IO_READ_DATA,
    W25Q_IO_IDENTIFY,
    W25Q_IO_WAIT_READY,
    W25Q_IO_RECOVERY
} W25Q_IoStage;

typedef struct
{
    W25Q_IoStage stage;
    uint32_t address;
    uint32_t hal_status;    /* HAL_OK=0, HAL_ERROR=1, HAL_BUSY=2, HAL_TIMEOUT=3. */
    uint32_t hal_error;
    uint32_t elapsed_ms;
    uint32_t read_retries;  /* Cumulative extra read attempts since W25Q_Init. */
} W25Q_Diagnostic;

/* Storage task is the sole owner. GPIO is configured in gpio.c.
 * Supports identified Winbond 3V W25Q parts up to 128Mbit (24-bit addresses).
 * No chip erase API; every operation has a bounded timeout. */
uint8_t W25Q_Init(void);
void W25Q_GetInfo(W25Q_Info *info);
/* Last transport/BUSY failure remains available even after successful recovery. */
void W25Q_GetDiagnostic(W25Q_Diagnostic *diagnostic);
uint8_t W25Q_Read(uint32_t address, void *data, uint32_t length);
uint8_t W25Q_Program(uint32_t address, const void *data, uint32_t length);
uint8_t W25Q_EraseSector(uint32_t address);

#endif
