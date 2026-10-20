#include "settings_store.h"
#include "stm32f1xx_hal.h"

/* Real settings driver; mock only erase/program HAL at mapped Flash addresses.
 * Fixtures use Python zlib's independent CRC32 implementation. */
#define CHECK(group, condition) do { if (!(condition)) { failures |= (1UL << (group)); } } while (0)
#define PAGE0 SETTINGS_FLASH_START_ADDRESS
#define PAGE1 (SETTINGS_FLASH_START_ADDRESS + SETTINGS_FLASH_PAGE_SIZE)
static uint32_t failures;
static uint32_t erase_calls, program_calls, unlock_calls, lock_calls;
static uint32_t fail_program_at, corrupt_program_at;
static uint8_t fail_unlock, fail_erase, fail_lock, unlocked;
static uint32_t bad_access;
static const uint16_t rollover_fixture[13] = { 0xC35AU, 0x51E7U, 0x0001U, 0x0008U, 0xFFFFU, 0xFFFFU, 0x012CU, 0x0118U, 0x02BCU, 0x028AU, 0xC653U, 0x249CU, 0xA55AU };
static const uint16_t invalid_fixture[13] = { 0xC35AU, 0x51E7U, 0x0001U, 0x0008U, 0x0001U, 0x0000U, 0x01F5U, 0x01E1U, 0x02BCU, 0x028AU, 0x964AU, 0x10DCU, 0xA55AU };

static volatile uint16_t *flash(uint32_t address) { return (volatile uint16_t *)address; }
static uint8_t address_valid(uint32_t address)
{
    return (uint8_t)((address >= PAGE0) && (address < PAGE1 + SETTINGS_FLASH_PAGE_SIZE) && ((address & 1U) == 0U));
}
void test_clear_flash_flags(uint32_t flags)
{
    if ((flags != (FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR)) || (unlocked == 0U)) { ++bad_access; }
}
HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    ++unlock_calls;
    if (fail_unlock != 0U) { return HAL_ERROR; }
    unlocked = 1U;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Lock(void)
{
    ++lock_calls;
    unlocked = 0U;
    return (fail_lock != 0U) ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *page_error)
{
    uint32_t offset;
    ++erase_calls;
    if ((unlocked == 0U) || (erase->TypeErase != FLASH_TYPEERASE_PAGES) ||
        (erase->NbPages != 1U) || ((erase->PageAddress != PAGE0) && (erase->PageAddress != PAGE1)))
    { ++bad_access; return HAL_ERROR; }
    if (fail_erase != 0U) { *page_error = erase->PageAddress; return HAL_ERROR; }
    for (offset = 0U; offset < SETTINGS_FLASH_PAGE_SIZE; offset += 2U) { *flash(erase->PageAddress + offset) = 0xFFFFU; }
    *page_error = 0xFFFFFFFFUL;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data)
{
    ++program_calls;
    if ((unlocked == 0U) || (type != FLASH_TYPEPROGRAM_HALFWORD) || (address_valid(address) == 0U) ||
        (data > 0xFFFFU) || ((*flash(address) & (uint16_t)data) != (uint16_t)data))
    { ++bad_access; return HAL_ERROR; }
    if (program_calls == fail_program_at) { return HAL_ERROR; }
    *flash(address) &= (uint16_t)data;
    if (program_calls == corrupt_program_at) { *flash(address) ^= 1U; }
    return HAL_OK;
}
static void reset_counts(void)
{
    erase_calls = 0U; program_calls = 0U; unlock_calls = 0U; lock_calls = 0U;
    fail_program_at = 0U; corrupt_program_at = 0U;
    fail_unlock = 0U; fail_erase = 0U; fail_lock = 0U; unlocked = 0U;
}
static void blank_flash(void)
{
    uint32_t offset;
    for (offset = 0U; offset < 2U * SETTINGS_FLASH_PAGE_SIZE; offset += 2U) { *flash(PAGE0 + offset) = 0xFFFFU; }
    reset_counts();
}
static void set_config(EnvAlarm_Config *config, uint16_t temperature, uint16_t humidity)
{
    config->temperature_on_x10 = temperature;
    config->temperature_off_x10 = temperature - ENV_ALARM_TEMP_HYSTERESIS_X10;
    config->humidity_on_x10 = humidity;
    config->humidity_off_x10 = humidity - ENV_ALARM_HUM_HYSTERESIS_X10;
}
static uint8_t matches(const EnvAlarm_Config *config, uint16_t temperature, uint16_t humidity)
{
    return (uint8_t)((config->temperature_on_x10 == temperature) &&
                    (config->temperature_off_x10 == temperature - ENV_ALARM_TEMP_HYSTERESIS_X10) &&
                    (config->humidity_on_x10 == humidity) &&
                    (config->humidity_off_x10 == humidity - ENV_ALARM_HUM_HYSTERESIS_X10));
}

uint32_t RunSettingsStoreTests(void)
{
    EnvAlarm_Config current;
    EnvAlarm_Config loaded;
    uint32_t index;
    uint32_t offset;
    uint16_t saved;
    uint16_t header_indices[5] = {0U, 1U, 2U, 3U, 12U};

    blank_flash();
    set_config(&loaded, 350U, 800U);
    CHECK(0, SettingsStore_Load(&loaded) == SETTINGS_STORE_EMPTY && matches(&loaded, 350U, 800U));
    CHECK(0, SettingsStore_Load(0) == SETTINGS_STORE_ERROR);
    CHECK(0, SettingsStore_Save(0) == SETTINGS_STORE_ERROR && erase_calls == 0U);

    set_config(&current, 350U, 800U);
    CHECK(1, SettingsStore_Save(&current) == SETTINGS_STORE_OK);
    CHECK(1, erase_calls == 1U && program_calls == 13U && unlock_calls == 1U && lock_calls == 1U && unlocked == 0U);
    CHECK(1, *flash(PAGE0 + 24U) == 0xA55AU && *flash(PAGE1) == 0xFFFFU);
    CHECK(1, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 350U, 800U));
    reset_counts();
    CHECK(1, SettingsStore_Save(&current) == SETTINGS_STORE_UNCHANGED);
    CHECK(1, unlock_calls == 0U && erase_calls == 0U && program_calls == 0U);

    set_config(&current, 400U, 850U);
    CHECK(2, SettingsStore_Save(&current) == SETTINGS_STORE_OK);
    CHECK(2, *flash(PAGE0 + 12U) == 350U && *flash(PAGE1 + 12U) == 400U);
    set_config(&current, 450U, 900U);
    CHECK(2, SettingsStore_Save(&current) == SETTINGS_STORE_OK);
    CHECK(2, *flash(PAGE0 + 12U) == 450U && *flash(PAGE1 + 12U) == 400U);
    CHECK(2, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 450U, 900U));

    *flash(PAGE0 + 12U) ^= 1U;
    CHECK(3, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 400U, 850U));
    *flash(PAGE1 + 12U) ^= 1U;
    CHECK(3, SettingsStore_Load(&loaded) == SETTINGS_STORE_EMPTY && matches(&loaded, 400U, 850U));

    blank_flash();
    set_config(&current, 350U, 800U);
    (void)SettingsStore_Save(&current);
    for (index = 0U; index < 5U; ++index)
    {
        offset = 2U * header_indices[index];
        saved = *flash(PAGE0 + offset);
        *flash(PAGE0 + offset) ^= 1U;
        CHECK(4, SettingsStore_Load(&loaded) == SETTINGS_STORE_EMPTY);
        *flash(PAGE0 + offset) = saved;
    }
    for (index = 0U; index < 13U; ++index) { *flash(PAGE0 + 2U * index) = invalid_fixture[index]; }
    CHECK(4, SettingsStore_Load(&loaded) == SETTINGS_STORE_EMPTY); /* Valid CRC, invalid threshold. */

    blank_flash();
    set_config(&current, 350U, 800U);
    (void)SettingsStore_Save(&current);
    set_config(&current, 400U, 850U);
    reset_counts(); fail_unlock = 1U;
    CHECK(5, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR && erase_calls == 0U && unlocked == 0U);
    reset_counts(); fail_erase = 1U;
    CHECK(5, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR && lock_calls == 1U && unlocked == 0U);
    CHECK(5, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 350U, 800U));

    for (index = 1U; index <= 13U; ++index)
    {
        blank_flash();
        set_config(&current, 350U, 800U);
        (void)SettingsStore_Save(&current);
        set_config(&current, 400U, 850U);
        reset_counts(); fail_program_at = index;
        CHECK(6, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR && unlocked == 0U);
        CHECK(6, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 350U, 800U));
        CHECK(6, *flash(PAGE1 + 24U) != 0xA55AU);
    }

    blank_flash(); fail_program_at = 13U;
    CHECK(7, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR);
    set_config(&loaded, 300U, 700U);
    CHECK(7, SettingsStore_Load(&loaded) == SETTINGS_STORE_EMPTY && matches(&loaded, 300U, 700U));

    blank_flash();
    set_config(&current, 350U, 800U);
    (void)SettingsStore_Save(&current);
    set_config(&current, 400U, 850U);
    reset_counts(); corrupt_program_at = 7U;
    CHECK(8, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR && program_calls == 7U);
    CHECK(8, *flash(PAGE1 + 24U) == 0xFFFFU && unlocked == 0U);
    CHECK(8, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 350U, 800U));

    blank_flash();
    for (index = 0U; index < 13U; ++index) { *flash(PAGE0 + 2U * index) = rollover_fixture[index]; }
    CHECK(9, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 300U, 700U));
    set_config(&current, 400U, 850U);
    CHECK(9, SettingsStore_Save(&current) == SETTINGS_STORE_OK);
    CHECK(9, *flash(PAGE1 + 8U) == 0U && *flash(PAGE1 + 10U) == 0U);
    CHECK(9, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 400U, 850U));

    reset_counts();
    current.temperature_on_x10 = 501U;
    CHECK(10, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR);
    set_config(&current, 400U, 850U); ++current.humidity_off_x10;
    CHECK(10, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR);
    CHECK(10, erase_calls == 0U && unlock_calls == 0U);
    CHECK(10, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 400U, 850U));

    blank_flash(); set_config(&current, 350U, 800U);
    (void)SettingsStore_Save(&current);
    set_config(&current, 400U, 850U);
    reset_counts(); corrupt_program_at = 13U;
    CHECK(11, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR && unlocked == 0U);
    CHECK(11, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 350U, 800U));
    reset_counts(); fail_lock = 1U;
    CHECK(11, SettingsStore_Save(&current) == SETTINGS_STORE_ERROR);
    CHECK(11, SettingsStore_Load(&loaded) == SETTINGS_STORE_OK && matches(&loaded, 400U, 850U));
    reset_counts();
    CHECK(11, SettingsStore_Save(&current) == SETTINGS_STORE_UNCHANGED && erase_calls == 0U);
    CHECK(11, bad_access == 0U);
    return failures;
}
