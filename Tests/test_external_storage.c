#include "settings_store.h"
#include "history_store.h"
#include "storage_codec.h"
#include "w25qxx.h"
#include "time_service.h"
#include <string.h>

/* Boundary model: NOR permits only 1->0 programming; erase is sector aligned.
 * Program budget injects a power cut at each byte, including the commit. */
static uint8_t media[4U * 4096U];
static uint8_t saved_media[sizeof(media)];
static int32_t program_budget;
static uint8_t erase_failure;
static uint8_t read_failure;
static uint32_t read_failure_address;
static uint8_t media_ready = 1U;
static uint32_t media_capacity = sizeof(media);
static uint32_t erase_count;
static uint32_t failures;
#define CHECK(group, condition) do { if (!(condition)) { failures |= 1UL << (group); } } while (0)

void W25Q_GetInfo(W25Q_Info *info)
{
    info->ready = media_ready;
    info->capacity_bytes = media_capacity;
    info->jedec_id = 0xEF4016UL;
}
uint8_t W25Q_Read(uint32_t address, void *data, uint32_t length)
{
    if ((read_failure != 0U) || (address == read_failure_address) ||
        (address >= sizeof(media)) || (length > sizeof(media) - address)) { return 0U; }
    (void)memcpy(data, media + address, length);
    return 1U;
}
uint8_t W25Q_Program(uint32_t address, const void *data, uint32_t length)
{
    const uint8_t *input = (const uint8_t *)data;
    if ((address >= sizeof(media)) || (length > sizeof(media) - address)) { return 0U; }
    while (length-- > 0U)
    {
        if (program_budget == 0) { return 0U; }
        if (program_budget > 0) { --program_budget; }
        if ((media[address] & *input) != *input) { return 0U; }
        media[address++] &= *input++;
    }
    return 1U;
}
uint8_t W25Q_EraseSector(uint32_t address)
{
    if ((erase_failure != 0U) || ((address % 4096U) != 0U) || (address >= sizeof(media))) { return 0U; }
    (void)memset(media + address, 0xFF, 4096U);
    ++erase_count;
    return 1U;
}

static void reset_media(void)
{
    (void)memset(media, 0xFF, sizeof(media));
    program_budget = -1;
    erase_failure = 0U;
    read_failure = 0U;
    read_failure_address = 0xFFFFFFFFUL;
    media_ready = 1U;
    media_capacity = sizeof(media);
    erase_count = 0U;
}

uint32_t RunExternalStorageTests(void)
{
    EnvAlarm_Config first = {350U,330U,800U,750U};
    EnvAlarm_Config second = {400U,380U,850U,800U};
    EnvAlarm_Config loaded;
    History_Sample sample;
    History_Record record;
    History_Info info;
    History_Diagnostic diagnostic;
    TimeService_Snapshot anchor;
    uint32_t count;
    uint32_t timestamp;
    uint8_t bytes[32];
    uint8_t copy[32];
    char text[20];

    failures = 0U;
    CHECK(0, StorageCodec_Crc32((const uint8_t *)"123456789", 9U) == 0xCBF43926UL);
    StorageCodec_Put32(bytes, 0x78563412UL);
    CHECK(0, (bytes[0] == 0x12U) && (StorageCodec_Get32(bytes) == 0x78563412UL));
    reset_media();
    loaded = first;
    CHECK(1, (SettingsStore_Load(&loaded) == SETTINGS_STORE_EMPTY) && (loaded.temperature_on_x10 == 350U));
    CHECK(1, SettingsStore_Save(&first) == SETTINGS_STORE_OK);
    CHECK(1, (SettingsStore_Load(&loaded) == SETTINGS_STORE_OK) && (loaded.humidity_on_x10 == 800U));
    CHECK(1, (SettingsStore_Save(&first) == SETTINGS_STORE_UNCHANGED) && (erase_count == 1U));
    CHECK(2, SettingsStore_Save(&second) == SETTINGS_STORE_OK);
    media[4096U + 8U] ^= 1U;
    CHECK(2, (SettingsStore_Load(&loaded) == SETTINGS_STORE_OK) && (loaded.temperature_on_x10 == 350U));
    reset_media();
    (void)SettingsStore_Save(&first);
    (void)memcpy(saved_media, media, sizeof(media));
    for (count = 0U; count < 32U; ++count)
    {
        (void)memcpy(media, saved_media, sizeof(media));
        program_budget = (int32_t)count;
        CHECK(3, SettingsStore_Save(&second) == SETTINGS_STORE_ERROR);
        CHECK(3, (SettingsStore_Load(&loaded) == SETTINGS_STORE_OK) && (loaded.temperature_on_x10 == 350U));
    }
    program_budget = -1;
    erase_failure = 1U;
    CHECK(4, SettingsStore_Save(&second) == SETTINGS_STORE_ERROR);
    erase_failure = 0U;
    second.temperature_on_x10 = 600U;
    count = erase_count;
    CHECK(4, (SettingsStore_Save(&second) == SETTINGS_STORE_ERROR) && (erase_count == count));

    reset_media();
    (void)SettingsStore_Save(&first);
    (void)memcpy(copy, media, 32U);
    (void)memset(&sample, 0, sizeof(sample));
    sample.sample.valid = 1U;
    sample.sample.temperature_x10 = 286U;
    sample.sample.humidity_x10 = 650U;
    sample.time.calibrated = 1U;
    sample.time.timestamp = 1792395000UL;
    sample.time.uptime_seconds = 10U;
    sample.alarm_flags = 3U;
    CHECK(5, HistoryStore_Open() != 0U);
    CHECK(5, HistoryStore_ReadRecent(0U, &record) == 0U);
    CHECK(5, HistoryStore_Append(&sample) != 0U);
    CHECK(5, HistoryStore_ReadRecent(0U, &record) != 0U);
    CHECK(5, (record.sequence == 1U) && (record.session == 1U) && (record.value.sample.valid == 1U) &&
             (record.value.sample.temperature_x10 == 286U) && (record.value.sample.humidity_x10 == 650U) &&
             (record.value.alarm_flags == 3U) && (record.value.time.timestamp == sample.time.timestamp));
    CHECK(6, HistoryStore_Open() != 0U);
    HistoryStore_GetInfo(&info);
    CHECK(6, (info.session == 2U) && (info.next_sequence == 2U) && (info.slot_capacity == 254U));
    sample.time.calibrated = 0U;
    sample.sample.valid = 0U;
    CHECK(6, HistoryStore_Append(&sample) != 0U);
    CHECK(6, HistoryStore_ReadRecent(0U, &record) != 0U);
    CHECK(6, (record.session == 2U) && (record.value.time.calibrated == 0U) &&
             (record.value.time.timestamp == 0U) && (record.value.sample.valid == 0U));
    (void)memcpy(saved_media, media, sizeof(media));
    for (count = 0U; count < 32U; ++count)
    {
        (void)memcpy(media, saved_media, sizeof(media));
        program_budget = -1;
        CHECK(7, HistoryStore_Open() != 0U);
        program_budget = (int32_t)count;
        CHECK(7, HistoryStore_Append(&sample) == 0U);
        program_budget = -1;
        CHECK(7, HistoryStore_Open() != 0U);
        CHECK(7, (HistoryStore_ReadRecent(0U, &record) != 0U) && (record.sequence == 2U));
        CHECK(7, HistoryStore_Append(&sample) != 0U);
        CHECK(7, (HistoryStore_ReadRecent(0U, &record) != 0U) && (record.sequence >= 3U));
    }
    reset_media();
    (void)SettingsStore_Save(&first);
    (void)HistoryStore_Open();
    for (count = 0U; count < 300U; ++count)
    {
        sample.time.uptime_seconds = count;
        CHECK(8, HistoryStore_Append(&sample) != 0U);
    }
    CHECK(8, (HistoryStore_ReadRecent(0U, &record) != 0U) && (record.sequence == 300U));
    CHECK(8, (HistoryStore_ReadRecent(172U, &record) != 0U) && (record.sequence == 128U));
    CHECK(8, HistoryStore_ReadRecent(173U, &record) == 0U);
    CHECK(8, memcmp(copy, media, 32U) == 0);
    CHECK(9, HistoryStore_Open() != 0U);
    CHECK(9, (HistoryStore_ReadRecent(0U, &record) != 0U) && (record.sequence == 300U));
    media[2U * 4096U + 46U * 32U + 20U] ^= 1U;
    CHECK(9, (HistoryStore_ReadRecent(0U, &record) != 0U) && (record.sequence == 299U));
    read_failure = 1U;
    CHECK(13, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_ERROR);
    read_failure = 0U;
    CHECK(13, HistoryStore_ReadRecent(0U, NULL) == HISTORY_READ_ERROR);
    reset_media();
    media[2U * 4096U] = 0x12U;
    CHECK(10, (HistoryStore_Open() == 0U) && (erase_count == 0U));
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(10, diagnostic.error == HISTORY_ERROR_HEADER_UNKNOWN && diagnostic.address == 8192U &&
              diagnostic.invalid_headers == 1U && diagnostic.header_magic == 0xFFFFFF12UL &&
              diagnostic.stored_crc == 0xFFFFFFFFUL && diagnostic.commit == 0xFFFFFFFFUL &&
              diagnostic.computed_crc == StorageCodec_Crc32(media + 8192U, 24U));

    CHECK(11, TimeService_Parse("2026 10 19 15:30:00", &timestamp) != 0U);
    CHECK(11, timestamp == 1792395000UL);
    TimeService_Format(timestamp, text);
    CHECK(11, strcmp(text, "2026 10 19 15:30:00") == 0);
    CHECK(11, TimeService_Parse("2024 02 29 23:59:59", &timestamp) != 0U);
    TimeService_Format(timestamp + 1U, text);
    CHECK(11, strcmp(text, "2024 03 01 00:00:00") == 0);
    /* Date separators are spaces; reject malformed or old input safely. */
    CHECK(12, TimeService_Parse("2026-10-19 15:30:00", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026  10 19 15:30:00", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026 10 19 15:30", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026 1x 19 15:30:00", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026 02 29 00:00:00", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026 13 01 00:00:00", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026 10 19 24:00:00", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026 10 19 00:00:60", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2026 10 19 00:00:00x", &timestamp) == 0U);
    CHECK(12, TimeService_Parse("2099 12 31 23:59:59", &timestamp) != 0U);
    TimeService_Format(timestamp, text);
    CHECK(12, strcmp(text, "2099 12 31 23:59:59") == 0);

    /* Calibration immediately resolves earlier samples, without rewriting them. */
    reset_media();
    (void)memset(&sample, 0, sizeof(sample));
    sample.sample.valid = 1U;
    sample.time.uptime_seconds = 20U;
    anchor.timestamp = 1792395000UL;
    anchor.uptime_seconds = 80U;
    anchor.calibrated = 1U;
    CHECK(14, HistoryStore_Open() != 0U && HistoryStore_Append(&sample) != 0U);
    CHECK(14, HistoryStore_AppendTimeAnchor(&anchor) != 0U);
    (void)memcpy(saved_media, media, sizeof(media));
    CHECK(14, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK);
    CHECK(14, record.sequence == 1U && record.time_source == HISTORY_TIME_ESTIMATED &&
              record.display_timestamp == anchor.timestamp - 60U &&
              record.value.time.timestamp == 0U && record.value.time.calibrated == 0U);
    CHECK(14, HistoryStore_ReadRecent(1U, &record) == HISTORY_READ_EMPTY);
    CHECK(14, memcmp(saved_media, media, sizeof(media)) == 0);

    CHECK(15, HistoryStore_Open() != 0U);
    CHECK(15, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_ESTIMATED && record.display_timestamp == 1792394940UL);
    HistoryStore_GetInfo(&info);
    CHECK(15, info.session == 3U && info.next_sequence == 3U);

    /* A previous boot's anchor cannot date samples in this boot. */
    sample.time.uptime_seconds = 10U;
    CHECK(16, HistoryStore_Append(&sample) != 0U);
    CHECK(16, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_UNKNOWN);
    anchor.timestamp = 1792396000UL;
    anchor.uptime_seconds = 40U;
    CHECK(16, HistoryStore_AppendTimeAnchor(&anchor) != 0U);
    CHECK(16, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_ESTIMATED && record.display_timestamp == 1792395970UL);
    CHECK(16, HistoryStore_ReadRecent(1U, &record) == HISTORY_READ_OK &&
              record.session == 1U && record.display_timestamp == 1792394940UL);

    /* Repeated calibration does not relabel earlier RTC-stamped data. */
    anchor.timestamp += 600U;
    anchor.uptime_seconds = 50U;
    CHECK(17, HistoryStore_AppendTimeAnchor(&anchor) != 0U);
    CHECK(17, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.display_timestamp == 1792395970UL);
    sample.time = anchor;
    sample.time.uptime_seconds += 10U;
    sample.time.timestamp += 10U;
    CHECK(17, HistoryStore_Append(&sample) != 0U);
    CHECK(17, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_RTC && record.display_timestamp == sample.time.timestamp);
    CHECK(17, HistoryStore_ReadRecent(1U, &record) == HISTORY_READ_OK &&
              record.display_timestamp == 1792395970UL);

    reset_media();
    (void)HistoryStore_Open();
    sample.time.calibrated = 0U;
    sample.time.uptime_seconds = 20U;
    (void)HistoryStore_Append(&sample);
    count = erase_count;
    CHECK(18, HistoryStore_AppendTimeAnchor(NULL) == 0U);
    anchor.calibrated = 0U;
    CHECK(18, HistoryStore_AppendTimeAnchor(&anchor) == 0U && erase_count == count);
    anchor.calibrated = 1U;
    anchor.uptime_seconds = 10U;
    (void)HistoryStore_AppendTimeAnchor(&anchor);
    CHECK(18, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_UNKNOWN);
    anchor.uptime_seconds = 80U;
    anchor.timestamp = 1577808000UL;
    (void)HistoryStore_AppendTimeAnchor(&anchor);
    CHECK(18, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_UNKNOWN);

    /* Each incomplete anchor must remain invisible after a simulated reboot. */
    reset_media();
    (void)HistoryStore_Open();
    (void)HistoryStore_Append(&sample);
    (void)memcpy(saved_media, media, sizeof(media));
    anchor.timestamp = 1792395000UL;
    for (count = 0U; count < 32U; ++count)
    {
        (void)memcpy(media, saved_media, sizeof(media));
        program_budget = -1;
        (void)HistoryStore_Open();
        CHECK(19, HistoryStore_Append(&sample) != 0U);
        program_budget = (int32_t)count;
        CHECK(19, HistoryStore_AppendTimeAnchor(&anchor) == 0U);
        program_budget = -1;
        CHECK(19, HistoryStore_Open() != 0U);
        CHECK(19, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
                  record.sequence == 2U && record.time_source == HISTORY_TIME_UNKNOWN);
    }

    /* The previous format's later valid sample is also a usable anchor. */
    reset_media();
    (void)HistoryStore_Open();
    (void)HistoryStore_Append(&sample);
    sample.time = anchor;
    (void)HistoryStore_Append(&sample);
    CHECK(20, HistoryStore_ReadRecent(1U, &record) == HISTORY_READ_OK &&
              record.display_timestamp == 1792394940UL && record.time_source == HISTORY_TIME_ESTIMATED);

    reset_media();
    (void)HistoryStore_Open();
    sample.time.calibrated = 0U;
    for (count = 0U; count < 127U; ++count)
    {
        sample.time.uptime_seconds = count;
        CHECK(21, HistoryStore_Append(&sample) != 0U);
    }
    anchor.uptime_seconds = 200U;
    CHECK(21, HistoryStore_AppendTimeAnchor(&anchor) != 0U);
    CHECK(21, HistoryStore_Open() != 0U);
    CHECK(21, HistoryStore_ReadRecent(126U, &record) == HISTORY_READ_OK &&
              record.sequence == 1U && record.display_timestamp == anchor.timestamp - 200U);

    reset_media();
    (void)HistoryStore_Open();
    sample.time.uptime_seconds = 20U;
    (void)HistoryStore_Append(&sample);
    (void)HistoryStore_AppendTimeAnchor(&anchor);
    media[2U * 4096U + 2U * 32U + 16U] ^= 1U;
    CHECK(22, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK &&
              record.time_source == HISTORY_TIME_UNKNOWN);

    reset_media();
    (void)HistoryStore_Open();
    CHECK(23, HistoryStore_AppendTimeAnchor(&anchor) != 0U);
    CHECK(23, HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_EMPTY);
    CHECK(23, HistoryStore_Open() != 0U);
    HistoryStore_GetInfo(&info);
    CHECK(23, info.session == 2U && info.next_sequence == 2U);

    /* Reproduce ready=0/slots=0/next=1 with distinct scan failures. */
    reset_media();
    read_failure_address = 12288U;
    CHECK(24, HistoryStore_Open() == 0U);
    HistoryStore_GetInfo(&info);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(24, info.ready == 0U && info.slot_capacity == 0U && info.next_sequence == 1U &&
              diagnostic.error == HISTORY_ERROR_READ_HEADER && diagnostic.address == 12288U &&
              erase_count == 0U);

    reset_media();
    (void)HistoryStore_Open();
    (void)HistoryStore_Append(&sample);
    count = erase_count;
    read_failure_address = 8224U;
    CHECK(25, HistoryStore_Open() == 0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(25, diagnostic.error == HISTORY_ERROR_READ_SLOT && diagnostic.address == 8224U && erase_count == count);

    reset_media();
    (void)HistoryStore_Open();
    erase_failure = 1U;
    CHECK(26, HistoryStore_Append(&sample) == 0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(26, diagnostic.error == HISTORY_ERROR_ERASE_SECTOR && diagnostic.address == 8192U && erase_count == 0U);

    reset_media();
    (void)HistoryStore_Open();
    program_budget = 10;
    CHECK(27, HistoryStore_Append(&sample) == 0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(27, diagnostic.error == HISTORY_ERROR_WRITE_HEADER && diagnostic.address == 8192U);

    reset_media();
    (void)HistoryStore_Open();
    (void)HistoryStore_Append(&sample);
    program_budget = 10;
    CHECK(28, HistoryStore_Append(&sample) == 0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    HistoryStore_GetInfo(&info);
    CHECK(28, diagnostic.error == HISTORY_ERROR_WRITE_RECORD && diagnostic.address == 8256U &&
              info.ready == 0U && info.next_sequence == 2U);

    reset_media();
    media_ready = 0U;
    CHECK(29, HistoryStore_Open() == 0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(29, diagnostic.error == HISTORY_ERROR_MEDIA_NOT_READY && erase_count == 0U);
    media_ready = 1U;
    media_capacity = 3U * 4096U;
    CHECK(29, HistoryStore_Open() == 0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(29, diagnostic.error == HISTORY_ERROR_MEDIA_NOT_READY && erase_count == 0U);

    reset_media();
    (void)HistoryStore_Open();
    (void)HistoryStore_Append(&sample);
    media[12288U] = 0x12U;
    CHECK(30, HistoryStore_Open() != 0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(30, diagnostic.error == HISTORY_ERROR_NONE &&
              HistoryStore_ReadRecent(0U, &record) == HISTORY_READ_OK);

    /* First-header cut points must retain the existing no-auto-format policy. */
    for (count = 0U; count < 32U; ++count)
    {
        reset_media();
        (void)HistoryStore_Open();
        program_budget = (int32_t)count;
        CHECK(31, HistoryStore_Append(&sample) == 0U);
        program_budget = -1;
        CHECK(31, (HistoryStore_Open() != 0U) == (count == 0U));
        HistoryStore_GetDiagnostic(&diagnostic);
        CHECK(31, erase_count == 1U && diagnostic.error ==
                  ((count == 0U) ? HISTORY_ERROR_NONE : HISTORY_ERROR_HEADER_UNKNOWN));
    }
    return failures;
}
