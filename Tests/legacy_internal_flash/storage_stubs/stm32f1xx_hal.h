#ifndef STORAGE_TEST_HAL_H
#define STORAGE_TEST_HAL_H
#include <stdint.h>
typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t TypeErase; uint32_t Banks; uint32_t PageAddress; uint32_t NbPages; } FLASH_EraseInitTypeDef;
#define FLASH_TYPEERASE_PAGES 0U
#define FLASH_TYPEPROGRAM_HALFWORD 1U
#define FLASH_FLAG_EOP 0x20U
#define FLASH_FLAG_PGERR 0x04U
#define FLASH_FLAG_WRPERR 0x10U
void test_clear_flash_flags(uint32_t flags);
#define __HAL_FLASH_CLEAR_FLAG(flags) test_clear_flash_flags(flags)
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *page_error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data);
#endif
