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
static uint32_t reads;
static uint32_t fail_on_read;
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
    ++reads;
    if ((fail_on_read != 0U && reads == fail_on_read) || (read_failure != 0U) || (address == read_failure_address) ||
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
    erase_count = reads = fail_on_read = 0U;
}


static void seed(uint32_t count)
{
    History_Sample sample = {{250U,600U,1U},{0U,10U,0U},0U};
    while (count-- > 0U) { (void)HistoryStore_Append(&sample); }
}
uint32_t RunHistoryClearTests(void)
{
    EnvAlarm_Config config = {350U,330U,800U,750U};
    EnvAlarm_Config loaded;
    History_Record record;
    History_Info info;
    History_Reader reader;
    History_Diagnostic diagnostic;
    History_Sample sample = {{251U,601U,1U},{0U,200U,0U},0U};
    uint8_t header[32] = {0U};
    uint32_t index, next, before, seen;
    failures = 0U;

    reset_media(); (void)HistoryStore_Open();
    CHECK(0, HistoryStore_Clear()!=0U && HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);
    HistoryStore_GetInfo(&info);
    CHECK(0, info.ready!=0U && info.slot_capacity==254U && info.next_sequence==1U && erase_count==1U);

    reset_media(); (void)SettingsStore_Save(&config); (void)HistoryStore_Open(); seed(40U);
    HistoryStore_GetInfo(&info); next=info.next_sequence; before=erase_count;
    (void)memcpy(saved_media,media,sizeof(media));
    (void)HistoryStore_BeginRead(&reader,0U);
    CHECK(1, HistoryStore_Clear()!=0U && erase_count==before+1U && memcmp(saved_media,media,8192U)==0);
    CHECK(1, SettingsStore_Load(&loaded)==SETTINGS_STORE_OK && loaded.temperature_on_x10==350U);
    CHECK(2, HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY &&
             memcmp(media+8192U,saved_media+8192U,4096U)==0);
    CHECK(3, HistoryStore_ReadNext(&reader,&record,8U)==HISTORY_READ_OVERRUN);
    CHECK(4, HistoryStore_Open()!=0U && HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);
    HistoryStore_GetInfo(&info); CHECK(4, info.next_sequence==next);
    CHECK(5, HistoryStore_Append(&sample)!=0U && HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_OK &&
             record.sequence==next && record.time_source==HISTORY_TIME_UNKNOWN &&
             HistoryStore_ReadRecent(1U,&record)==HISTORY_READ_EMPTY);

    seed(127U); CHECK(6, HistoryStore_Open()!=0U);
    CHECK(6, HistoryStore_ReadRecent(127U,&record)==HISTORY_READ_OK && record.sequence==next &&
             HistoryStore_ReadRecent(128U,&record)==HISTORY_READ_EMPTY);
    before=erase_count;
    CHECK(7, HistoryStore_Clear()!=0U && erase_count==before+1U &&
             HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);
    CHECK(7, HistoryStore_Open()!=0U && HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY &&
             memcmp(saved_media,media,8192U)==0);

    /* Every interrupted header-program point: no false success/auto erase. */
    reset_media(); (void)SettingsStore_Save(&config); (void)HistoryStore_Open(); seed(40U);
    (void)memcpy(saved_media,media,sizeof(media));
    for(index=0U;index<32U;++index)
    {
        (void)memcpy(media,saved_media,sizeof(media)); program_budget=-1; (void)HistoryStore_Open();
        program_budget=(int32_t)index;
        CHECK(8, HistoryStore_Clear()==0U && memcmp(media,saved_media,8192U)==0);
        HistoryStore_GetInfo(&info); CHECK(8, info.ready==0U);
        program_budget=-1; before=erase_count;
        CHECK(8, HistoryStore_Open()!=0U && erase_count==before &&
                 HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_OK && record.sequence==40U);
    }
    (void)memcpy(media,saved_media,sizeof(media)); (void)HistoryStore_Open(); program_budget=32;
    CHECK(9, HistoryStore_Clear()!=0U); program_budget=-1;
    CHECK(9, HistoryStore_Open()!=0U && HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);

    (void)memcpy(media,saved_media,sizeof(media)); (void)HistoryStore_Open(); erase_failure=1U;
    CHECK(10, HistoryStore_Clear()==0U);
    HistoryStore_GetDiagnostic(&diagnostic);
    CHECK(10, diagnostic.error==HISTORY_ERROR_ERASE_SECTOR && diagnostic.address==12288U);
    erase_failure=0U;
    CHECK(10, HistoryStore_Open()!=0U && HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_OK);

    /* Commit reached media but verification read was lost: report failure;
     * reboot accepts the committed boundary, never replay an uncertain write. */
    (void)memcpy(media,saved_media,sizeof(media)); (void)HistoryStore_Open(); fail_on_read=reads+2U;
    CHECK(11, HistoryStore_Clear()==0U); fail_on_read=0U;
    CHECK(11, HistoryStore_Open()!=0U && HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);

    reset_media(); media_ready=0U; (void)HistoryStore_Open(); before=erase_count;
    CHECK(12, HistoryStore_Clear()==0U && erase_count==before);

    /* Generation zero is valid after wrap; the explicit tag distinguishes it. */
    reset_media();
    StorageCodec_Put32(header,0x3144484CUL); StorageCodec_Put32(header+4U,0xFFFFFFFFUL);
    StorageCodec_Put32(header+8U,1U); (void)StorageCodec_Write(8192U,header);
    (void)HistoryStore_Open(); seed(1U);
    CHECK(13, HistoryStore_Clear()!=0U && HistoryStore_Open()!=0U &&
              HistoryStore_ReadRecent(0U,&record)==HISTORY_READ_EMPTY);
    seed(128U); (void)HistoryStore_Open();
    CHECK(13, HistoryStore_ReadRecent(127U,&record)==HISTORY_READ_OK && record.sequence==2U &&
              HistoryStore_ReadRecent(128U,&record)==HISTORY_READ_EMPTY);

    reset_media(); (void)HistoryStore_Open(); (void)HistoryStore_Clear();
    (void)HistoryStore_BeginRead(&reader,0U);
    CHECK(14, HistoryStore_ReadNext(&reader,&record,8U)==HISTORY_READ_EMPTY &&
              HistoryStore_ReadNext(&reader,&record,8U)==HISTORY_READ_EMPTY);

    reset_media(); (void)HistoryStore_Open(); (void)HistoryStore_Clear();
    seed(300U); CHECK(15, HistoryStore_Open()!=0U);
    (void)HistoryStore_BeginRead(&reader,0U); seen=0U;
    for(index=0U;index<400U;++index)
    {
        History_ReadStatus status=HistoryStore_ReadNext(&reader,&record,8U);
        if(status==HISTORY_READ_EMPTY) { break; }
        if(status==HISTORY_READ_OK) { ++seen; CHECK(15, record.sequence<=300U); }
        else { CHECK(15,status==HISTORY_READ_PENDING); }
    }
    CHECK(15, seen==173U); /* The latest 46 plus the preceding 127 slots. */
    return failures;
}
