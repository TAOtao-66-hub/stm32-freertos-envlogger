#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

/* 项目参数入口：本文件只定义常量，不定义变量，也不依赖 HAL/FreeRTOS。
 * GPIO 接线与模式见 gpio.h / gpio.c；内核参数见 FreeRTOSConfig.h。
 * 协议命令、位掩码、字模等实现常量留在对应驱动中。 */

/* 1. 采样时序（单位：ms）。通常只修改应用周期，保持不小于驱动限制。
 * 1000ms 下限依据本项目采用的 DHT11 手册；更换器件时核对实际规格。 */
#define ENV_MONITOR_SAMPLE_INTERVAL_MS  1100U  /* 应用发起采样的间隔 */
#define DHT11_MIN_READ_INTERVAL_MS      1000U  /* 驱动拒绝过密读取的下限 */
#define ENV_MONITOR_POWER_UP_DELAY_MS   100U   /* 显示初始化前的上电等待 */
#define ENV_ALARM_SAMPLE_TIMEOUT_MS     6000U  /* 报警任务等待采样消息的上限 */

/* 2. 报警上电默认值（单位：0.1℃、0.1%RH）。运行时可通过 set 修改。
 * 回差宽度由默认触发值减解除值得到；启动优先恢复有效保存记录，无记录才用默认值。 */
#define ENV_ALARM_TEMP_ON_X10           300U
#define ENV_ALARM_TEMP_OFF_X10          280U
#define ENV_ALARM_HUM_ON_X10            700U
#define ENV_ALARM_HUM_OFF_X10           650U
#define ENV_ALARM_TEMP_HYSTERESIS_X10   (ENV_ALARM_TEMP_ON_X10 - ENV_ALARM_TEMP_OFF_X10)
#define ENV_ALARM_HUM_HYSTERESIS_X10    (ENV_ALARM_HUM_ON_X10 - ENV_ALARM_HUM_OFF_X10)
#define ENV_ALARM_TEMP_SET_MAX_X10      500U   /* DHT11 温度设置上限：50.0℃ */
#define ENV_ALARM_HUM_SET_MAX_X10       1000U  /* 湿度设置上限：100.0%RH */

/* 3. 按键（单位：ms）。 */
#define BUTTON_DEBOUNCE_MS              20U

/* 4. 任务调度。栈深度单位为 32 位 word；256 words = 1024 字节。
 * 优先级数值越大越先运行，必须小于 FreeRTOS 的 configMAX_PRIORITIES。 */
#define ENV_MONITOR_TASK_STACK_WORDS    256U
#define ENV_MONITOR_TASK_PRIORITY       3U
#define ENV_MONITOR_POLL_INTERVAL_MS    10U
#define ALARM_TASK_STACK_WORDS          256U
#define ALARM_TASK_PRIORITY             4U
#define ALARM_POLL_INTERVAL_MS          10U

/* 5. USART1：115200、8N1、无流控；接收队列存单字节，短报文在串口任务中发送。
 * 每次采样完成上报一次，周期跟随 ENV_MONITOR_SAMPLE_INTERVAL_MS；不另设定时器。 */
#define SERIAL_BAUD_RATE               115200U
#define SERIAL_IRQ_PRIORITY            6U
#define SERIAL_RX_QUEUE_LENGTH         128U
#define SERIAL_TX_MAX_BYTES            128U
#define SERIAL_TX_TIMEOUT_MS           50U
#define SERIAL_TASK_STACK_WORDS        384U
#define SERIAL_TASK_PRIORITY           2U
#define SERIAL_RX_WAIT_MS              20U
#define SERIAL_RETRY_DELAY_MS          10U
#define SERIAL_COMMAND_MAX_LENGTH       32U   /* 命令最大字符数，不含行结束与结尾零 */
#define SERIAL_COMMAND_IDLE_MS         100U   /* 无新字节达到此时长，自动结束当前命令 */
/* GET 的样本接收超时复用 ENV_ALARM_SAMPLE_TIMEOUT_MS。 */


/* 6. External W25Q: fixed geometry for the supported 3V EF40xx family. */
#define FLASH_SECTOR_BYTES              4096U
#define FLASH_PAGE_BYTES                256U
/* HAL counts wall time, including preemption by the initial OLED refresh. */
#define FLASH_SPI_TRANSFER_TIMEOUT_MS   250U
#define FLASH_READ_MAX_ATTEMPTS         3U     /* Read only; never replay a write/erase. */
#define FLASH_READ_RETRY_DELAY_MS       1U
#define FLASH_PROGRAM_TIMEOUT_MS        20U
#define FLASH_ERASE_TIMEOUT_MS          1000U
#define FLASH_POWER_UP_DELAY_MS         10U
#define HISTORY_RECORD_INTERVAL_MS      10000U
#define HISTORY_RECORD_QUEUE_LENGTH     8U     /* FIFO; full drops NEW record, never blocks alarm. */
#define STORAGE_TASK_STACK_WORDS        384U
#define STORAGE_TASK_PRIORITY           1U
#define STORAGE_POLL_INTERVAL_MS        10U
#define STORAGE_REQUEST_QUEUE_LENGTH    4U
#define STORAGE_TEXT_QUEUE_LENGTH       4U
#define STORAGE_REPLY_TIMEOUT_MS        200U
#define STORAGE_COMMAND_TIMEOUT_MS      10000U
#define HISTORY_PAGE_MAX_RECORDS        20U
#define HISTORY_SCAN_BUDGET_SLOTS        8U    /* Bound each export scan step. */
#define HISTORY_TRANSFER_PROGRESS_MS    1000U /* Internal keepalive; never printed. */
#define LOG_CLEAR_CONFIRM_WINDOW_MS     10000U

/* RAM diagnostic warning margins, in bytes; lifetime minima, not CPU load. */
#define RTOS_HEAP_WARNING_BYTES         256U
#define RTOS_STACK_WARNING_BYTES        128U

/* 7. RTC uses internal LSI; PC sets local UTC+8 time after each startup. */
#define TIME_LOCAL_UTC_OFFSET_SECONDS   28800U
#define TIME_RTC_TIMEOUT_MS             100U

/* 8. B5K -> PA0 ADC1_IN0; current mode defaults to OFF (remote control). */
#define POT_SAMPLE_INTERVAL_MS          100U
#define POT_ADC_TIMEOUT_MS              2U
#define POT_TEMPERATURE_STEP_X10        5U
#define POT_HUMIDITY_STEP_X10           10U

#if ENV_MONITOR_SAMPLE_INTERVAL_MS < DHT11_MIN_READ_INTERVAL_MS
#error Sampling period must meet the DHT11 minimum interval.
#endif
#if ENV_ALARM_SAMPLE_TIMEOUT_MS <= ENV_MONITOR_SAMPLE_INTERVAL_MS
#error Alarm timeout must exceed the sample period.
#endif
#if HISTORY_RECORD_INTERVAL_MS < ENV_MONITOR_SAMPLE_INTERVAL_MS
#error History interval must be no shorter than the sampling interval.
#endif
#if (POT_TEMPERATURE_STEP_X10 == 0U) || (POT_HUMIDITY_STEP_X10 == 0U)
#error Potentiometer mapping steps must be positive.
#endif
#if FLASH_READ_MAX_ATTEMPTS < 1U
#error Flash reads require at least one attempt.
#endif
#if (HISTORY_RECORD_QUEUE_LENGTH < 1U) || (HISTORY_RECORD_QUEUE_LENGTH > 16U)
#error History FIFO length must be 1..16; check the F103 RAM budget before enlarging.
#endif
#if (HISTORY_SCAN_BUDGET_SLOTS < 2U) || (HISTORY_PAGE_MAX_RECORDS < 1U) || (HISTORY_PAGE_MAX_RECORDS > 20U)
#error Invalid bounded history transfer configuration.
#endif
#if (LOG_CLEAR_CONFIRM_WINDOW_MS == 0U) || (LOG_CLEAR_CONFIRM_WINDOW_MS >= 0x80000000UL)
#error Clear confirmation window must be positive and below half the tick range.
#endif
#if (RTOS_HEAP_WARNING_BYTES == 0U) || (RTOS_STACK_WARNING_BYTES == 0U)
#error Resource warning margins must be positive byte counts.
#endif
#endif /* PROJECT_CONFIG_H */
