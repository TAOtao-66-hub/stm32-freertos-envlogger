#include "w25qxx.h"
#include "potentiometer.h"
#include "project_commands.h"
#include "serial_port.h"
#include <string.h>
/* UART command routing only; actual kernel diagnostics have a separate suite. */
static uint32_t diagnostic_calls;
HAL_StatusTypeDef RtosDiagnostics_Print(void) { ++diagnostic_calls; return HAL_OK; }

static uint8_t media[8192];
static uint32_t ticks;
static uint8_t selected, command, wel, bad_id, busy_forever, transport_error, adc_error;
static uint32_t address, program_calls, page_errors, adc_raw, received_ready;
static uint8_t spi_config_ok;
static Storage_Request queued;
static Storage_TextReply received;
static char output[1024];
static uint32_t sends;
static uint8_t queue_full;
static TimeService_Snapshot fake_time;
static uint32_t failures;
static uint32_t read_calls, recover_calls, read_failures, fail_read_call, status_failures, command_failures;
static uint8_t recovery_failure, spi_poisoned;
static uint32_t preemption_ms;
#define CHECK(group, condition) do { if (!(condition)) { failures |= 1UL << (group); } } while (0)

uint32_t HAL_GetTick(void) { return ticks++; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *spi)
{
    spi->ErrorCode = 0U;
    spi_poisoned = 0U;
    spi_config_ok = ((spi->Instance == SPI1) && (spi->Init.BaudRatePrescaler == 64U) &&
                      (spi->Init.CLKPolarity == 0U) && (spi->Init.CLKPhase == 0U)) ? 1U : 0U;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *spi)
{
    (void)spi;
    ++recover_calls;
    return ((selected == 0U) && (recovery_failure == 0U)) ? HAL_OK : HAL_ERROR;
}
void HAL_GPIO_WritePin(GPIO_TypeDef *port,uint16_t pin,GPIO_PinState state)
{
    (void)port; (void)pin;
    selected = (state == GPIO_PIN_RESET) ? 1U : 0U;
    if (selected != 0U) { command = 0U; }
    else if ((command == 0x02U) || (command == 0x20U)) { wel = 0U; }
}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi,uint8_t *data,uint16_t length,uint32_t timeout)
{
    uint16_t index;
    (void)spi; (void)timeout;
    if ((selected == 0U) || (transport_error != 0U)) { return HAL_ERROR; }
    if (command == 0x02U)
    {
        if ((wel == 0U) || ((address % 256U) + length > 256U)) { ++page_errors; return HAL_ERROR; }
        ++program_calls;
        for (index=0U;index<length;++index) { if (address < sizeof(media)) { media[address] &= data[index]; } ++address; }
        return HAL_OK;
    }
    command = data[0];
    if ((command == 0x03U) && (command_failures > 0U))
    { --command_failures; ticks += timeout; spi_poisoned = 1U; return HAL_TIMEOUT; }
    if (command == 0x06U) { wel = 1U; }
    if (length == 4U)
    {
        address = ((uint32_t)data[1]<<16U)|((uint32_t)data[2]<<8U)|data[3];
        if (command == 0x20U)
        {
            if ((wel == 0U) || (address >= sizeof(media))) { return HAL_ERROR; }
            (void)memset(media + address,0xFF,4096U);
        }
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *spi,uint8_t *tx,uint8_t *rx,uint16_t length,uint32_t timeout)
{
    uint16_t index;
    (void)spi; (void)timeout;
    if ((selected == 0U) || (transport_error != 0U)) { return HAL_ERROR; }
    if (spi_poisoned != 0U) { return HAL_ERROR; }
    if (command == 0x03U)
    {
        ++read_calls;
        if ((read_failures > 0U) || (read_calls == fail_read_call))
        {
            if (read_failures > 0U) { --read_failures; }
            rx[0] = 0x00U; /* Partial stale output must not survive the retry. */
            ticks += timeout;
            spi_poisoned = 1U;
            return HAL_TIMEOUT;
        }
        if (preemption_ms != 0U)
        {
            ticks += preemption_ms;
            if (preemption_ms >= timeout) { preemption_ms = 0U; return HAL_TIMEOUT; }
            preemption_ms = 0U;
        }
        for(index=0U;index<length;++index) { rx[index]=(address<sizeof(media))?media[address]:0xFFU; ++address; }
    }
    else if ((tx[0] == 0x9FU) && (length == 4U))
    { rx[0]=0U; rx[1]=(bad_id!=0U)?0xFFU:0xEFU; rx[2]=0x40U; rx[3]=0x14U; }
    else if ((tx[0] == 0x05U) && (length == 2U))
    {
        if (status_failures > 0U) { --status_failures; ticks += timeout; spi_poisoned = 1U; return HAL_TIMEOUT; }
        rx[0]=0U; rx[1]=(uint8_t)((wel!=0U?2U:0U)|(busy_forever!=0U?1U:0U));
    }
    else { return HAL_ERROR; }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *adc) { (void)adc; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *adc,ADC_ChannelConfTypeDef *channel)
{ (void)adc; return (channel->Channel==0U && channel->SamplingTime==239U)?HAL_OK:HAL_ERROR; }
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *adc) { (void)adc; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *adc) { (void)adc; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *adc) { (void)adc; return HAL_OK; }
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *adc,uint32_t timeout)
{ (void)adc; (void)timeout; return adc_error!=0U?HAL_TIMEOUT:HAL_OK; }
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *adc) { (void)adc; return adc_raw; }

BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t delay)
{
    (void)delay;
    if ((queue != (void *)1) || (queue_full != 0U)) { return pdFAIL; }
    queued = *(const Storage_Request *)item;
    ++sends;
    return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t delay)
{
    (void)delay;
    if ((queue != (void *)2) || (received_ready == 0U)) { return pdFAIL; }
    *(Storage_TextReply *)item = received;
    received_ready = 0U;
    return pdPASS;
}
HAL_StatusTypeDef SerialPort_WriteString(const char *text)
{
    if (strlen(output)+strlen(text)>=sizeof(output)) { return HAL_ERROR; }
    (void)strcat(output,text);
    return HAL_OK;
}
void TimeService_Get(TimeService_Snapshot *snapshot) { *snapshot=fake_time; }
uint8_t TimeService_SetText(const char *text)
{ fake_time.calibrated=TimeService_Parse(text,&fake_time.timestamp); return fake_time.calibrated; }

uint32_t RunExternalPeripheralTests(void)
{
    W25Q_Info info;
    W25Q_Diagnostic diagnostic;
    uint8_t data[20], result[20], handled;
    uint8_t larger[70];
    uint32_t index, start, previous_id, before;
    EnvAlarm_Request request;
    Pot_Snapshot pot;

    failures=0U;
    (void)memset(media,0xFF,sizeof(media));
    CHECK(0,W25Q_Init()!=0U);
    W25Q_GetInfo(&info);
    CHECK(0,spi_config_ok!=0U && info.jedec_id==0xEF4014UL && info.capacity_bytes==1048576UL);
    for(index=0U;index<sizeof(data);++index) { data[index]=(uint8_t)index; }
    CHECK(1,W25Q_Program(250U,data,sizeof(data))!=0U);
    CHECK(1,program_calls==2U && page_errors==0U && selected==0U);
    CHECK(1,W25Q_Read(250U,result,sizeof(result))!=0U && memcmp(data,result,sizeof(data))==0);
    CHECK(2,W25Q_EraseSector(1U)==0U && W25Q_Read(1048570U,result,20U)==0U);
    CHECK(2,W25Q_Program(0xFFFFFFF0UL,data,20U)==0U && W25Q_Read(0U,NULL,1U)==0U);
    CHECK(2,W25Q_EraseSector(0U)!=0U && media[250U]==0xFFU);
    transport_error=1U;
    CHECK(3,W25Q_Read(0U,result,1U)==0U && selected==0U);
    transport_error=0U;
    busy_forever=1U; start=ticks;
    CHECK(3,W25Q_Program(0U,data,1U)==0U && ticks-start<=1100U && selected==0U);
    busy_forever=0U; bad_id=1U;
    CHECK(4,W25Q_Init()==0U);
    CHECK(4,W25Q_Program(0U,data,1U)==0U);
    bad_id=0U;
    CHECK(4,W25Q_Init()!=0U);

    CHECK(5,Pot_Init()!=0U);
    adc_raw=0U;
    CHECK(5,Pot_Poll(100U,&request)==0U);
    CHECK(5,Pot_SetMode(POT_MODE_TEMPERATURE)!=0U && Pot_Poll(200U,&request)!=0U);
    CHECK(5,request.kind==ENV_ALARM_REQUEST_SET_TEMPERATURE && request.temperature_x10==20U);
    adc_raw=4095U;
    for(index=3U;index<=100U;++index) { (void)Pot_Poll(index*100U,&request); }
    CHECK(6,request.temperature_x10==500U);
    CHECK(6,Pot_SetMode(POT_MODE_HUMIDITY)!=0U && Pot_Poll(10100U,&request)!=0U && request.humidity_x10==1000U);
    CHECK(6,Pot_SetMode((Pot_Mode)3)==0U);
    CHECK(7,Pot_Poll(10200U,&request)==0U);
    adc_error=1U;
    CHECK(7,Pot_Poll(10300U,&request)==0U);
    adc_error=0U;
    CHECK(7,Pot_SetMode(POT_MODE_OFF)!=0U && Pot_Poll(10400U,&request)==0U);
    Pot_Get(&pot);
    CHECK(7,pot.ready!=0U && pot.mode==POT_MODE_OFF && pot.raw==4095U);

    ProjectCommands_Bind((void *)1,(void *)2);
    output[0]='\0';
    CHECK(8,ProjectCommands_Execute("time get",&handled)==HAL_OK && handled!=0U && strstr(output,"UNCALIBRATED")!=NULL);
    output[0]='\0';
    CHECK(8,ProjectCommands_Execute("time set 2026 10 19 15:30:00",&handled)==HAL_OK &&
            queued.kind==STORAGE_REQUEST_TIME_SET && queued.timestamp==1792395000UL &&
            output[0]=='\0' && fake_time.calibrated==0U);
    received.request_id=queued.request_id; received.final=1U;
    (void)strcpy(received.text,"OK time set; beijingtime; anchor=STORED\r\n");
    received_ready=1U; ProjectCommands_Service();
    CHECK(8,strstr(output,"anchor=STORED")!=NULL);
    fake_time.timestamp=queued.timestamp;
    fake_time.calibrated=1U;
    output[0]='\0';
    (void)ProjectCommands_Execute("time get",&handled);
    CHECK(8,strstr(output,"2026 10 19 15:30:00 beijingtime")!=NULL);
    output[0]='\0';
    (void)ProjectCommands_Execute("pot get",&handled);
    CHECK(8,strstr(output,"mode=off")!=NULL);
    (void)ProjectCommands_Execute("unknown",&handled);
    CHECK(8,handled==0U);

    output[0]='\0'; sends=0U;
    (void)ProjectCommands_Execute("log read 0",&handled);
    (void)ProjectCommands_Execute("log read 21",&handled);
    (void)ProjectCommands_Execute("log read 2x",&handled);
    CHECK(9,sends==0U);
    output[0]='\0';
    (void)ProjectCommands_Execute("log read 20",&handled);
    CHECK(9,sends==1U && queued.kind==STORAGE_REQUEST_READ && queued.count==20U);
    previous_id=queued.request_id;
    (void)ProjectCommands_Execute("flash info",&handled);
    CHECK(9,sends==1U && strstr(output,"STORAGE_BUSY")!=NULL);
    received.request_id=previous_id+1U; received.final=1U; (void)strcpy(received.text,"wrong\r\n");
    received_ready=1U; ProjectCommands_Service();
    CHECK(10,strstr(output,"wrong")==NULL);
    received.request_id=previous_id; received.final=1U;
    received_ready=1U; ProjectCommands_Service();
    (void)ProjectCommands_Execute("flash info",&handled);
    CHECK(10,sends==2U && queued.request_id!=previous_id);
    ticks+=10001U; ProjectCommands_Service();
    CHECK(10,strstr(output,"STORAGE_TIMEOUT")!=NULL);
    output[0]='\0';
    queue_full=1U;
    (void)ProjectCommands_Execute("log status",&handled);
    CHECK(11,strstr(output,"STORAGE_BUSY")!=NULL);
    queue_full=0U;
    (void)ProjectCommands_Execute("log status",&handled);
    CHECK(11,sends==3U && queued.kind==STORAGE_REQUEST_STATUS);
    /* Reads are restartable. Writes and erases must never be blindly replayed. */
    CHECK(12, W25Q_Init() != 0U);
    recover_calls = read_calls = 0U;
    read_failures = 1U;
    CHECK(12, W25Q_Read(128U, result, sizeof(result)) != 0U);
    W25Q_GetDiagnostic(&diagnostic);
    CHECK(12, recover_calls == 1U && read_calls == 2U && selected == 0U &&
              diagnostic.stage == W25Q_IO_READ_DATA && diagnostic.hal_status == HAL_TIMEOUT &&
              diagnostic.address == 128U && diagnostic.read_retries == 1U);
    for (index = 0U; index < sizeof(result); ++index) { CHECK(12, result[index] == 0xFFU); }

    for (index = 0U; index < sizeof(larger); ++index) { media[512U + index] = (uint8_t)(index + 1U); }
    read_calls = 0U;
    fail_read_call = 2U;
    CHECK(13, W25Q_Read(512U, larger, sizeof(larger)) != 0U &&
              memcmp(larger, media + 512U, sizeof(larger)) == 0 && read_calls == 5U);
    fail_read_call = 0U;

    recover_calls = read_calls = 0U;
    read_failures = FLASH_READ_MAX_ATTEMPTS;
    CHECK(14, W25Q_Read(0U, result, sizeof(result)) == 0U);
    CHECK(14, read_calls == FLASH_READ_MAX_ATTEMPTS &&
              recover_calls + 1U == FLASH_READ_MAX_ATTEMPTS && selected == 0U);

    /* The next independent read can recover from the previous exhausted call. */
    CHECK(15, W25Q_Init() != 0U);
    recover_calls = 0U;
    command_failures = 1U;
    CHECK(15, W25Q_Read(256U, result, sizeof(result)) != 0U && recover_calls == 1U);
    W25Q_GetDiagnostic(&diagnostic);
    CHECK(15, diagnostic.stage == W25Q_IO_TRANSMIT && diagnostic.hal_status == HAL_TIMEOUT);

    status_failures = 1U;
    CHECK(16, W25Q_Read(256U, result, sizeof(result)) != 0U);
    W25Q_GetDiagnostic(&diagnostic);
    CHECK(16, diagnostic.stage == W25Q_IO_STATUS && diagnostic.hal_status == HAL_TIMEOUT);

    recover_calls = 0U;
    recovery_failure = 1U;
    read_failures = 1U;
    CHECK(17, W25Q_Read(0U, result, sizeof(result)) == 0U && recover_calls == 1U && selected == 0U);
    W25Q_GetDiagnostic(&diagnostic);
    CHECK(17, diagnostic.stage == W25Q_IO_RECOVERY && diagnostic.hal_status == HAL_ERROR);
    recovery_failure = 0U;
    CHECK(17, W25Q_Init() != 0U);

    recover_calls = 0U;
    busy_forever = 1U;
    start = ticks;
    CHECK(18, W25Q_Read(0U, result, sizeof(result)) == 0U && recover_calls == 0U && ticks - start < 1100U);
    W25Q_GetDiagnostic(&diagnostic);
    CHECK(18, diagnostic.stage == W25Q_IO_WAIT_READY && diagnostic.hal_status == HAL_OK);
    busy_forever = 0U;

    transport_error = 1U;
    CHECK(19, W25Q_Program(0U, data, sizeof(data)) == 0U && recover_calls == 0U && selected == 0U);
    CHECK(19, W25Q_EraseSector(0U) == 0U && recover_calls == 0U && selected == 0U);
    transport_error = 0U;

    CHECK(20, W25Q_Init() != 0U);
    preemption_ms = 180U; /* Initial OLED clear + drawing can exceed old 20 ms. */
    CHECK(20, W25Q_Read(256U, result, sizeof(result)) != 0U);
    W25Q_GetDiagnostic(&diagnostic);
    CHECK(20, diagnostic.stage == W25Q_IO_NONE && diagnostic.read_retries == 0U);

    before = read_calls;
    CHECK(21, W25Q_Read(0U, NULL, sizeof(result)) == 0U && read_calls == before);
    CHECK(21, W25Q_Read(1048570U, result, sizeof(result)) == 0U && read_calls == before);
    return failures;
}
