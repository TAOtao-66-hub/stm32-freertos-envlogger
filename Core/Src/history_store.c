#include "history_store.h"
#include "storage_codec.h"
#include "w25qxx.h"
#include <string.h>

#define HEADER_MAGIC  0x3144484CUL
#define LOG_MAGIC     0x31474F4CUL
#define TIME_MAGIC    0x314D4954UL
#define RECORD_BYTES  32U
#define SLOTS_PER_SECTOR (FLASH_SECTOR_BYTES / RECORD_BYTES)
#define FIRST_LOG_SECTOR 2U
#define CLEAR_BOUNDARY_MAGIC 0x31524C43UL /* CLR1 in previously reserved header fields. */
#define CLEAR_BOUNDARY_VERSION 1U

typedef struct
{
    History_Info info;
    History_Diagnostic diagnostic;
    uint32_t sector_count;
    uint32_t sector;
    uint32_t generation;
    uint32_t next_slot;
    uint32_t clear_generation;
    uint8_t has_header;
    uint8_t has_clear_boundary;
} History_Context;

static History_Context s_history;

static uint8_t fail_history(History_Error error, uint32_t sector, uint32_t slot,
                            const uint8_t *header)
{
    s_history.info.ready = 0U;
    s_history.diagnostic.error = error;
    s_history.diagnostic.address = sector * FLASH_SECTOR_BYTES + slot * RECORD_BYTES;
    s_history.diagnostic.header_magic = (header != NULL) ? StorageCodec_Get32(header) : 0U;
    s_history.diagnostic.stored_crc = (header != NULL) ? StorageCodec_Get32(header + 24U) : 0U;
    s_history.diagnostic.computed_crc = (header != NULL) ? StorageCodec_Crc32(header, 24U) : 0U;
    s_history.diagnostic.commit = (header != NULL) ? StorageCodec_Get32(header + 28U) : 0U;
    return 0U;
}

static uint8_t read_slot(uint32_t sector, uint32_t slot, uint8_t record[32])
{
    return W25Q_Read(sector * FLASH_SECTOR_BYTES + slot * RECORD_BYTES, record, RECORD_BYTES);
}

static uint8_t valid_header(const uint8_t header[32])
{
    if (StorageCodec_Valid(header, HEADER_MAGIC) == 0U) { return 0U; }
    if ((StorageCodec_Get32(header + 12U) == 0U) &&
        (StorageCodec_Get32(header + 16U) == 0U) && (StorageCodec_Get32(header + 20U) == 0U))
    { return 1U; } /* Original headers have zeroed reserved fields. */
    return ((StorageCodec_Get32(header + 16U) == CLEAR_BOUNDARY_MAGIC) &&
            (StorageCodec_Get32(header + 20U) == CLEAR_BOUNDARY_VERSION) &&
            ((int32_t)(StorageCodec_Get32(header + 4U) - StorageCodec_Get32(header + 12U)) >= 0)) ? 1U : 0U;
}

uint8_t HistoryStore_Open(void)
{
    W25Q_Info flash;
    uint8_t record[32];
    uint32_t sector;
    uint32_t slot;
    uint32_t generation;

    (void)memset(&s_history, 0, sizeof(s_history));
    W25Q_GetInfo(&flash);
    if ((flash.ready == 0U) || (flash.capacity_bytes < 4U * FLASH_SECTOR_BYTES))
    { return fail_history(HISTORY_ERROR_MEDIA_NOT_READY, 0U, 0U, NULL); }
    s_history.sector_count = flash.capacity_bytes / FLASH_SECTOR_BYTES;
    s_history.sector = FIRST_LOG_SECTOR;
    s_history.next_slot = 1U;
    s_history.info.next_sequence = 1U;
    for (sector = FIRST_LOG_SECTOR; sector < s_history.sector_count; ++sector)
    {
        if (read_slot(sector, 0U, record) == 0U)
        { return fail_history(HISTORY_ERROR_READ_HEADER, sector, 0U, NULL); }
        if (valid_header(record) == 0U)
        {
            if (StorageCodec_Blank(record, 32U) == 0U)
            {
                ++s_history.diagnostic.invalid_headers;
                /* Preserve the first unrecognized header for serial diagnosis. */
                if (s_history.diagnostic.invalid_headers == 1U)
                { (void)fail_history(HISTORY_ERROR_HEADER_UNKNOWN, sector, 0U, record); }
            }
            continue;
        }
        generation = StorageCodec_Get32(record + 4U);
        if ((s_history.has_header == 0U) || ((int32_t)(generation - s_history.generation) > 0))
        {
            s_history.has_header = 1U;
            s_history.generation = generation;
            s_history.sector = sector;
            s_history.info.next_sequence = StorageCodec_Get32(record + 8U);
            s_history.has_clear_boundary = (StorageCodec_Get32(record + 16U) == CLEAR_BOUNDARY_MAGIC) ? 1U : 0U;
            s_history.clear_generation = StorageCodec_Get32(record + 12U);
        }
    }
    /* Foreign/corrupt media is never silently formatted when no valid chain exists. */
    if ((s_history.has_header == 0U) && (s_history.diagnostic.invalid_headers != 0U)) { return 0U; }
    if (s_history.has_header != 0U)
    {
        for (slot = 1U; slot < SLOTS_PER_SECTOR; ++slot)
        {
            if (read_slot(s_history.sector, slot, record) == 0U)
            { return fail_history(HISTORY_ERROR_READ_SLOT, s_history.sector, slot, NULL); }
            if (StorageCodec_Blank(record, 32U) == 0U) { s_history.next_slot = slot + 1U; }
        }
        /* Reserve sequence numbers for torn slots too; never program them again. */
        s_history.info.next_sequence += s_history.next_slot - 1U;
    }
    s_history.info.session = s_history.info.next_sequence;
    s_history.info.slot_capacity = (s_history.sector_count - FIRST_LOG_SECTOR) * (SLOTS_PER_SECTOR - 1U);
    s_history.info.ready = 1U;
    s_history.diagnostic.error = HISTORY_ERROR_NONE;
    return 1U;
}

static uint8_t create_sector(uint32_t target, uint32_t generation,
                              uint8_t has_boundary, uint32_t boundary)
{
    uint8_t header[32] = {0U};
    StorageCodec_Put32(header, HEADER_MAGIC);
    StorageCodec_Put32(header + 4U, generation);
    StorageCodec_Put32(header + 8U, s_history.info.next_sequence);
    if (has_boundary != 0U)
    {
        StorageCodec_Put32(header + 12U, boundary);
        StorageCodec_Put32(header + 16U, CLEAR_BOUNDARY_MAGIC);
        StorageCodec_Put32(header + 20U, CLEAR_BOUNDARY_VERSION);
    }
    if (W25Q_EraseSector(target * FLASH_SECTOR_BYTES) == 0U)
    { return fail_history(HISTORY_ERROR_ERASE_SECTOR, target, 0U, NULL); }
    if (StorageCodec_Write(target * FLASH_SECTOR_BYTES, header) == 0U)
    { return fail_history(HISTORY_ERROR_WRITE_HEADER, target, 0U, NULL); }
    s_history.sector = target;
    s_history.generation = generation;
    s_history.next_slot = 1U;
    s_history.has_header = 1U;
    s_history.has_clear_boundary = has_boundary;
    s_history.clear_generation = boundary;
    return 1U;
}

static uint32_t next_sector(void)
{
    uint32_t target = s_history.sector;
    if (s_history.has_header != 0U)
    {
        ++target;
        if (target >= s_history.sector_count) { target = FIRST_LOG_SECTOR; }
    }
    return target;
}

static uint8_t start_sector(void)
{
    return create_sector(next_sector(), s_history.generation + 1U,
                         s_history.has_clear_boundary, s_history.clear_generation);
}

uint8_t HistoryStore_Clear(void)
{
    uint32_t generation;
    if (s_history.info.ready == 0U) { return 0U; }
    generation = s_history.generation + 1U;
    /* The committed header is the durable clear boundary. Keep global sequence
     * numbers monotonic; erase only one history sector, never settings 0/1. */
    if (create_sector(next_sector(), generation, 1U, generation) == 0U) { return 0U; }
    (void)memset(&s_history.diagnostic, 0, sizeof(s_history.diagnostic));
    return 1U;
}

static uint8_t append_record(uint8_t record[32])
{
    uint32_t address;
    if (s_history.info.ready == 0U) { return 0U; }
    if ((s_history.has_header == 0U) || (s_history.next_slot >= SLOTS_PER_SECTOR))
    {
        if (start_sector() == 0U) { return 0U; }
    }
    StorageCodec_Put32(record + 4U, s_history.info.next_sequence);
    StorageCodec_Put32(record + 8U, s_history.info.session);
    address = s_history.sector * FLASH_SECTOR_BYTES + s_history.next_slot * RECORD_BYTES;
    if (StorageCodec_Write(address, record) == 0U)
    {
        /* Require restart/recovery after uncertain writes. */
        return fail_history(HISTORY_ERROR_WRITE_RECORD, s_history.sector, s_history.next_slot, NULL);
    }
    ++s_history.next_slot;
    ++s_history.info.next_sequence;
    return 1U;
}

uint8_t HistoryStore_AppendTimeAnchor(const TimeService_Snapshot *time)
{
    uint8_t record[32] = {0U};

    if ((time == NULL) || (time->calibrated == 0U) ||
        (time->timestamp < 1577808000UL) || (time->timestamp > 4102415999UL)) { return 0U; }
    StorageCodec_Put32(record, TIME_MAGIC);
    StorageCodec_Put32(record + 12U, time->uptime_seconds);
    StorageCodec_Put32(record + 16U, time->timestamp);
    return append_record(record);
}

uint8_t HistoryStore_Append(const History_Sample *sample)
{
    uint8_t record[32] = {0U};

    if (sample == NULL) { return 0U; }
    StorageCodec_Put32(record, LOG_MAGIC);
    StorageCodec_Put32(record + 12U, sample->time.uptime_seconds);
    StorageCodec_Put32(record + 16U, (sample->time.calibrated != 0U) ? sample->time.timestamp : 0U);
    StorageCodec_Put16(record + 20U, (uint16_t)((sample->sample.temperature_x10 & 0x7FFFU) |
                       ((sample->sample.valid != 0U) ? 0x8000U : 0U)));
    StorageCodec_Put16(record + 22U, (uint16_t)((sample->sample.humidity_x10 & 0x07FFU) |
                       ((uint16_t)(sample->alarm_flags & 7U) << 11U) |
                       ((sample->time.calibrated != 0U) ? 0x8000U : 0U)));
    return append_record(record);
}

uint8_t HistoryStore_BeginRead(History_Reader *reader, uint32_t before_sequence)
{
    if ((reader == NULL) || (s_history.info.ready == 0U)) { return 0U; }
    (void)memset(reader, 0, sizeof(*reader));
    reader->sector = s_history.sector;
    reader->generation = s_history.generation;
    reader->slot = s_history.next_slot;
    reader->snapshot_sequence = s_history.info.next_sequence - 1U;
    reader->before_sequence = before_sequence;
    if (s_history.has_header != 0U)
    {
        reader->sectors_left = s_history.sector_count - FIRST_LOG_SECTOR;
        if (s_history.has_clear_boundary != 0U)
        {
            if (reader->generation - s_history.clear_generation + 1U < reader->sectors_left)
            { reader->sectors_left = reader->generation - s_history.clear_generation + 1U; }
        }
        else if ((reader->generation != 0U) && (reader->generation < reader->sectors_left))
        { reader->sectors_left = reader->generation; }
    }
    return 1U;
}

static void decode_record(const uint8_t bytes[32], const History_Reader *reader,
                          History_Record *output)
{
    uint16_t packed;
    uint32_t elapsed;
    output->sequence = StorageCodec_Get32(bytes + 4U);
    output->session = StorageCodec_Get32(bytes + 8U);
    output->value.time.uptime_seconds = StorageCodec_Get32(bytes + 12U);
    output->value.time.timestamp = StorageCodec_Get32(bytes + 16U);
    packed = StorageCodec_Get16(bytes + 20U);
    output->value.sample.valid = ((packed & 0x8000U) != 0U) ? 1U : 0U;
    output->value.sample.temperature_x10 = packed & 0x7FFFU;
    packed = StorageCodec_Get16(bytes + 22U);
    output->value.sample.humidity_x10 = packed & 0x07FFU;
    output->value.alarm_flags = (uint8_t)((packed >> 11U) & 7U);
    output->value.time.calibrated = ((packed & 0x8000U) != 0U) ? 1U : 0U;
    output->display_timestamp = output->value.time.timestamp;
    output->time_source = (output->value.time.calibrated != 0U) ? HISTORY_TIME_RTC : HISTORY_TIME_UNKNOWN;
    if ((output->time_source == HISTORY_TIME_UNKNOWN) && (reader->anchor.calibrated != 0U) &&
        (reader->anchor_session == output->session) &&
        (reader->anchor.uptime_seconds >= output->value.time.uptime_seconds))
    {
        elapsed = reader->anchor.uptime_seconds - output->value.time.uptime_seconds;
        if ((reader->anchor.timestamp >= elapsed) &&
            (reader->anchor.timestamp - elapsed >= 1577808000UL) &&
            (reader->anchor.timestamp - elapsed <= 4102415999UL))
        {
            output->display_timestamp = reader->anchor.timestamp - elapsed;
            output->time_source = HISTORY_TIME_ESTIMATED;
        }
    }
}

History_ReadStatus HistoryStore_ReadNext(History_Reader *reader, History_Record *output, uint32_t budget)
{
    uint8_t bytes[32];
    uint8_t header_checked = 0U;
    uint32_t sequence;
    uint32_t session;
    uint16_t packed;

    if ((reader == NULL) || (output == NULL) || (budget < 2U)) { return HISTORY_READ_ERROR; }
    if (reader->sectors_left == 0U) { return HISTORY_READ_EMPTY; }
    if ((s_history.has_clear_boundary != 0U) &&
        ((int32_t)(reader->generation - s_history.clear_generation) < 0))
    { return HISTORY_READ_OVERRUN; }
    while ((reader->sectors_left > 0U) && (budget > 0U))
    {
        /* Recheck on every call: the writer may recycle this sector between
         * calls. Never export a mixture of old and newly overwritten slots. */
        if (header_checked == 0U)
        {
            --budget;
            if (read_slot(reader->sector, 0U, bytes) == 0U) { return HISTORY_READ_ERROR; }
            if ((valid_header(bytes) == 0U) ||
                (StorageCodec_Get32(bytes + 4U) != reader->generation))
            { return HISTORY_READ_OVERRUN; }
            header_checked = 1U;
        }
        if (reader->slot <= 1U)
        {
            --reader->sectors_left;
            reader->sector = (reader->sector == FIRST_LOG_SECTOR) ?
                              s_history.sector_count - 1U : reader->sector - 1U;
            --reader->generation;
            reader->slot = SLOTS_PER_SECTOR;
            header_checked = 0U;
            continue;
        }
        if (budget == 0U) { break; }
        --budget;
        --reader->slot;
        if (read_slot(reader->sector, reader->slot, bytes) == 0U) { return HISTORY_READ_ERROR; }
        if ((StorageCodec_Valid(bytes, TIME_MAGIC) == 0U) &&
            (StorageCodec_Valid(bytes, LOG_MAGIC) == 0U))
        {
            if (StorageCodec_Blank(bytes, sizeof(bytes)) == 0U) { ++reader->skipped_invalid; }
            continue;
        }
        sequence = StorageCodec_Get32(bytes + 4U);
        if (sequence > reader->snapshot_sequence) { return HISTORY_READ_OVERRUN; }
        session = StorageCodec_Get32(bytes + 8U);
        packed = StorageCodec_Get16(bytes + 22U);
        if ((StorageCodec_Valid(bytes, TIME_MAGIC) != 0U) || ((packed & 0x8000U) != 0U))
        {
            reader->anchor_session = session;
            reader->anchor.uptime_seconds = StorageCodec_Get32(bytes + 12U);
            reader->anchor.timestamp = StorageCodec_Get32(bytes + 16U);
            reader->anchor.calibrated = 1U;
        }
        if (StorageCodec_Valid(bytes, LOG_MAGIC) == 0U) { continue; }
        if ((reader->before_sequence != 0U) && (sequence >= reader->before_sequence)) { continue; }
        decode_record(bytes, reader, output);
        return HISTORY_READ_OK;
    }
    return (reader->sectors_left == 0U) ? HISTORY_READ_EMPTY : HISTORY_READ_PENDING;
}

History_ReadStatus HistoryStore_ReadRecent(uint32_t offset, History_Record *output)
{
    History_Reader reader;
    History_ReadStatus status;
    if ((output == NULL) || (HistoryStore_BeginRead(&reader, 0U) == 0U)) { return HISTORY_READ_ERROR; }
    for (;;)
    {
        status = HistoryStore_ReadNext(&reader, output, HISTORY_SCAN_BUDGET_SLOTS);
        if (status == HISTORY_READ_PENDING) { continue; }
        if (status != HISTORY_READ_OK)
        { return (status == HISTORY_READ_OVERRUN) ? HISTORY_READ_ERROR : status; }
        if (offset == 0U) { return HISTORY_READ_OK; }
        --offset;
    }
}

void HistoryStore_GetInfo(History_Info *info)
{
    *info = s_history.info;
}

void HistoryStore_GetDiagnostic(History_Diagnostic *diagnostic)
{
    if (diagnostic != NULL) { *diagnostic = s_history.diagnostic; }
}
