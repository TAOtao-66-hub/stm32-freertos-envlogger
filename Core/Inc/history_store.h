#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H
#include "env_alarm.h"
#include "time_service.h"

typedef struct
{
    EnvAlarm_Sample sample;
    TimeService_Snapshot time;
    uint8_t alarm_flags;
} History_Sample;

typedef enum
{
    HISTORY_TIME_UNKNOWN = 0,
    HISTORY_TIME_RTC,
    HISTORY_TIME_ESTIMATED
} History_TimeSource;

typedef struct
{
    uint32_t sequence;
    uint32_t session;
    History_Sample value;
    uint32_t display_timestamp; /* Resolved date for output; raw value stays intact. */
    History_TimeSource time_source;
} History_Record;

typedef struct
{
    uint32_t slot_capacity;
    uint32_t next_sequence;
    uint32_t session;
    uint8_t ready;
} History_Info;

typedef enum
{
    HISTORY_ERROR_NONE = 0,
    HISTORY_ERROR_MEDIA_NOT_READY,
    HISTORY_ERROR_READ_HEADER,
    HISTORY_ERROR_HEADER_UNKNOWN,
    HISTORY_ERROR_READ_SLOT,
    HISTORY_ERROR_ERASE_SECTOR,
    HISTORY_ERROR_WRITE_HEADER,
    HISTORY_ERROR_WRITE_RECORD
} History_Error;

typedef struct
{
    History_Error error;
    uint32_t address;         /* Byte address in external W25Q, not STM32 Flash. */
    uint32_t invalid_headers;
    uint32_t header_magic;    /* Raw header fields: meaningful for HEADER_UNKNOWN only. */
    uint32_t stored_crc;
    uint32_t computed_crc;
    uint32_t commit;
} History_Diagnostic;

/* Sole owner: storageTask. Sectors 0/1 are exclusively settings;
 * each log sector has one committed header and 127 fixed 32-byte slots. */
uint8_t HistoryStore_Open(void);
uint8_t HistoryStore_Append(const History_Sample *sample);
/* Commit a new logical start; keep settings and global sequence intact.
 * Only one history sector is erased. Old bytes are reclaimed by ring writes.
 * On uncertain failure writes stop until Open/restart; never auto-replay. */
uint8_t HistoryStore_Clear(void);
/* Persist a calibration point in the same ring, with CRC and commit marker.
 * It belongs to the current boot session; it is not returned as a sample. */
uint8_t HistoryStore_AppendTimeAnchor(const TimeService_Snapshot *time);
/* offset=0 is newest valid record. Never loads the whole log into RAM. */
typedef enum
{
    HISTORY_READ_EMPTY = 0,
    HISTORY_READ_OK,
    HISTORY_READ_ERROR,
    HISTORY_READ_PENDING,
    HISTORY_READ_OVERRUN
} History_ReadStatus;
/* Private cursor fields; initialize with BeginRead and leave them unchanged.
 * A snapshot includes only records already committed when BeginRead runs. */
typedef struct
{
    uint32_t sector;
    uint32_t generation;
    uint32_t slot;
    uint32_t sectors_left;
    uint32_t snapshot_sequence;
    uint32_t before_sequence; /* Exclusive; zero starts at the newest record. */
    uint32_t anchor_session;
    uint32_t skipped_invalid;
    TimeService_Snapshot anchor;
} History_Reader;
uint8_t HistoryStore_BeginRead(History_Reader *reader, uint32_t before_sequence);
/* At most budget SPI slot reads. PENDING means call again; OVERRUN means
 * the writer has recycled an unread sector. All APIs belong to storageTask. */
History_ReadStatus HistoryStore_ReadNext(History_Reader *reader, History_Record *record, uint32_t budget);
History_ReadStatus HistoryStore_ReadRecent(uint32_t offset, History_Record *record);
void HistoryStore_GetInfo(History_Info *info);
/* Returns the last open/append failure; never reads, writes or erases Flash. */
void HistoryStore_GetDiagnostic(History_Diagnostic *diagnostic);
#endif
