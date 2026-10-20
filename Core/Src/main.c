/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "project_config.h"
#include "env_monitor.h"
#include "env_alarm.h"
#include "button.h"
#include "buzzer.h"
#include "led.h"
#include "serial_port.h"
#include "serial_report.h"
#include "serial_command.h"
#include "settings_store.h"
#include "storage_service.h"
#include "history_buffer.h"
#include "project_commands.h"
#include "rtos_diagnostics.h"
#include "time_service.h"
#include "potentiometer.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Fixed mailbox length: xQueueOverwrite requires a one-element queue.
 * Tunable task/sampling/alarm parameters are all in project_config.h. */
#define LATEST_VALUE_QUEUE_LENGTH     1U

#if (ENV_MONITOR_TASK_PRIORITY >= configMAX_PRIORITIES) || (ALARM_TASK_PRIORITY >= configMAX_PRIORITIES) || (SERIAL_TASK_PRIORITY >= configMAX_PRIORITIES) || (STORAGE_TASK_PRIORITY >= configMAX_PRIORITIES)
#error Application task priorities must be lower than configMAX_PRIORITIES.
#endif
#if (SERIAL_IRQ_PRIORITY < configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY) || (SERIAL_IRQ_PRIORITY > configLIBRARY_LOWEST_INTERRUPT_PRIORITY)
#error USART1 IRQ priority must permit FreeRTOS FromISR API calls.
#endif

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* File-private task handles. */
static TaskHandle_t s_env_monitor_task_handle = NULL;
static TaskHandle_t s_alarm_task_handle = NULL;
static TaskHandle_t s_serial_task_handle = NULL;
/* External Flash has a dedicated owner; acquisition needs no storage mutex. */
static TaskHandle_t s_storage_task_handle = NULL;
/* File-private queues; payloads are copied, not shared by pointer. */
static QueueHandle_t s_env_sample_queue = NULL;
static QueueHandle_t s_alarm_state_queue = NULL;
static QueueHandle_t s_serial_rx_queue = NULL;
/* Separate consumers need separate mailboxes; receiving removes an item. */
static QueueHandle_t s_serial_sample_queue = NULL;
/* One outstanding setting: request to alarmTask, applied result to serialTask. */
static QueueHandle_t s_alarm_request_queue = NULL;
static QueueHandle_t s_alarm_response_queue = NULL;

static QueueHandle_t s_storage_request_queue = NULL;
static QueueHandle_t s_storage_sample_queue = NULL;
static QueueHandle_t s_storage_boot_queue = NULL;
static QueueHandle_t s_storage_saved_queue = NULL;
static QueueHandle_t s_storage_text_queue = NULL;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void StartEnvMonitorTask(void *argument);
static void StartAlarmTask(void *argument);
static void StartSerialTask(void *argument);
static void StartStorageTask(void *argument);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  Own the DHT11 and OLED; publish samples and display alarm snapshots.
  * @param  argument: Unused task argument.
  * @note   Drivers use vTaskDelay, so initialize them after the scheduler starts.
  */
static void StartEnvMonitorTask(void *argument)
{
  EnvAlarm_Sample sample;
  EnvAlarm_State alarm_state = {0U};
  uint8_t refresh_needed;

  (void)argument;

  /* env_monitor.c includes oled.h; EnvMonitor_Init calls OLED_Init. */
  EnvMonitor_Init();

  for (;;)
  {
    refresh_needed = EnvMonitor_Poll();
    if (refresh_needed != 0U)
    {
      EnvMonitor_GetSample(&sample);

      /* Length-one queues keep the newest snapshot, without a stale backlog. */
      if (xQueueOverwrite(s_env_sample_queue, &sample) != pdPASS)
      {
        Error_Handler();
      }
      /* Publish a second copy: serialTask must not consume alarmTask's data. */
      if (xQueueOverwrite(s_serial_sample_queue, &sample) != pdPASS)
      {
        Error_Handler();
      }
    }

    if (xQueueReceive(s_alarm_state_queue, &alarm_state, 0U) == pdPASS)
    {
      EnvMonitor_SetAlarmState(&alarm_state);
      refresh_needed = 1U;
    }

    if (refresh_needed != 0U)
    {
      EnvMonitor_Refresh();
    }

    vTaskDelay(pdMS_TO_TICKS(ENV_MONITOR_POLL_INTERVAL_MS));
  }
}

/**
  * @brief  Own the button, LED and buzzer; apply hysteresis and mute rules.
  * @note   This task never accesses the OLED or the DHT11 directly.
  */
static void StartAlarmTask(void *argument)
{
  EnvAlarm_Sample sample = {0U, 0U, 0U};
  EnvAlarm_State state;
  EnvAlarm_State published;
  EnvAlarm_Request request;
  EnvAlarm_Request pot_request;
  EnvAlarm_Response response;
  EnvAlarm_Response pot_response;
  Storage_Boot boot;
  Storage_Request save;
  Storage_SaveReply saved;
  Pot_Snapshot pot;
  TickType_t last_sample_tick;
  uint8_t sample_available = 0U;
  uint8_t response_pending = 0U;
  uint8_t save_pending = 0U;
  uint8_t save_submitted = 0U;

  (void)argument;
  LED_Init();
  Buzzer_Init();
  Button_Init();
  EnvAlarm_Init(&state);
  /* Storage always publishes boot status, including absent/unsupported Flash. */
  if (xQueueReceive(s_storage_boot_queue, &boot, portMAX_DELAY) != pdPASS) { Error_Handler(); }
  if (boot.status == SETTINGS_STORE_OK) { state.config = boot.config; }
  published = state;
  (void)xQueueOverwrite(s_alarm_state_queue, &state);
  last_sample_tick = xTaskGetTickCount();
  HistoryBuffer_Init(s_storage_sample_queue, HAL_GetTick());

  for (;;)
  {
    if (xQueueReceive(s_env_sample_queue, &sample, 0U) == pdPASS)
    {
      last_sample_tick = xTaskGetTickCount();
      sample_available = 1U;
      EnvAlarm_Update(&state, &sample);
    }
    else if ((TickType_t)(xTaskGetTickCount() - last_sample_tick) >= pdMS_TO_TICKS(ENV_ALARM_SAMPLE_TIMEOUT_MS))
    {
      sample.valid = 0U;
      sample_available = 1U;
      EnvAlarm_Update(&state, &sample);
    }

    if ((response_pending == 0U) && (save_pending == 0U) &&
        (xQueueReceive(s_alarm_request_queue, &request, 0U) == pdPASS))
    {
      if (request.kind == ENV_ALARM_REQUEST_SAVE)
      {
        save.kind = STORAGE_REQUEST_SAVE;
        save.request_id = request.request_id;
        save.config = state.config;
        save.count = 0U;
        save.timestamp = 0U;
        save.before_sequence = 0U;
        save_pending = 1U;
        save_submitted = 0U;
      }
      else
      {
        Pot_Get(&pot);
        if ((request.kind >= ENV_ALARM_REQUEST_POT_OFF) && (request.kind <= ENV_ALARM_REQUEST_POT_HUM))
        {
          response.request_id = request.request_id;
          response.kind = request.kind;
          response.result = (Pot_SetMode((Pot_Mode)(request.kind - ENV_ALARM_REQUEST_POT_OFF)) != 0U) ?
                             ENV_ALARM_REQUEST_OK : ENV_ALARM_REQUEST_INVALID;
          response.config = state.config;
        }
        else if (((pot.mode == POT_MODE_TEMPERATURE) &&
                  ((request.kind == ENV_ALARM_REQUEST_SET_TEMPERATURE) || (request.kind == ENV_ALARM_REQUEST_SET_BOTH))) ||
                 ((pot.mode == POT_MODE_HUMIDITY) &&
                  ((request.kind == ENV_ALARM_REQUEST_SET_HUMIDITY) || (request.kind == ENV_ALARM_REQUEST_SET_BOTH))))
        {
          response.request_id = request.request_id;
          response.kind = request.kind;
          response.config = state.config;
          response.result = ENV_ALARM_REQUEST_LOCAL_CONTROL;
        }
        else
        {
          EnvAlarm_ApplyRequest(&state, &request, (sample_available != 0U) ? &sample : NULL, &response);
        }
        response_pending = 1U;
      }
    }
    if ((save_pending != 0U) && (save_submitted == 0U) &&
        (xQueueSend(s_storage_request_queue, &save, 0U) == pdPASS)) { save_submitted = 1U; }
    if ((save_pending != 0U) && (save_submitted != 0U) &&
        (xQueueReceive(s_storage_saved_queue, &saved, 0U) == pdPASS) && (saved.request_id == save.request_id))
    {
      response.request_id = save.request_id;
      response.kind = ENV_ALARM_REQUEST_SAVE;
      response.config = save.config;
      response.result = ENV_ALARM_REQUEST_STORAGE_ERROR;
      if (saved.status == SETTINGS_STORE_OK) { response.result = ENV_ALARM_REQUEST_OK; }
      if (saved.status == SETTINGS_STORE_UNCHANGED) { response.result = ENV_ALARM_REQUEST_SAVE_UNCHANGED; }
      response_pending = 1U;
      save_pending = 0U;
    }

    if (Pot_Poll(HAL_GetTick(), &pot_request) != 0U)
    {
      EnvAlarm_ApplyRequest(&state, &pot_request, (sample_available != 0U) ? &sample : NULL, &pot_response);
    }
    Pot_Get(&pot);
    state.control_mode = (uint8_t)pot.mode;
    if (Button_Poll() == BUTTON_EVENT_PRESSED) { EnvAlarm_Mute(&state); }
    /* Generate scheduled records even while storageTask is busy. FIFO send
     * never waits; latest-value mailboxes above keep their original role. */
    HistoryBuffer_Poll(HAL_GetTick(), (sample_available != 0U) ? &sample : NULL, state.flags);
    LED_Set((state.flags != 0U) ? 1U : 0U);
    Buzzer_Set(((state.flags != 0U) && (state.muted == 0U)) ? 1U : 0U);
    if ((state.flags != published.flags) || (state.muted != published.muted) ||
        (state.control_mode != published.control_mode) ||
        (state.config.temperature_on_x10 != published.config.temperature_on_x10) ||
        (state.config.humidity_on_x10 != published.config.humidity_on_x10))
    {
      (void)xQueueOverwrite(s_alarm_state_queue, &state);
      published = state;
    }
    if ((response_pending != 0U) && (xQueueSend(s_alarm_response_queue, &response, 0U) == pdPASS))
    { response_pending = 0U; }
    vTaskDelay(pdMS_TO_TICKS(ALARM_POLL_INTERVAL_MS));
  }
}

/**
  * @brief  Parse query/settings, forward applied replies and service USART1.
  * @note   Only this task owns UART writes and the command module's state.
  */
static void StartSerialTask(void *argument)
{
  uint8_t received_byte;
  uint8_t sample_received;
  BaseType_t byte_received;
  EnvAlarm_Sample sample;
  SerialPort_Stats stats;
  EnvAlarm_Response response;
  HAL_StatusTypeDef command_status;
  uint32_t last_dropped_count = 0U;
  uint32_t last_error_count = 0U;

  (void)argument;

  ProjectCommands_Bind(s_storage_request_queue, s_storage_text_queue);
  SerialCommand_Init();
  SerialCommand_BindAlarmQueue(s_alarm_request_queue);
  /* Arm RX only after the scheduler starts; its ISR can then wake this task. */
  if (SerialPort_StartReceive(s_serial_rx_queue) != HAL_OK)
  {
    Error_Handler();
  }
  if (SerialPort_WriteString("SYSTEM READY\r\n") != HAL_OK)
  {
    Error_Handler();
  }

  for (;;)
  {
    ProjectCommands_Service();
    if (SerialPort_Service() != HAL_OK)
    {
      /* RX recovery failure must not prevent reporting over a healthy TX. */
      vTaskDelay(pdMS_TO_TICKS(SERIAL_RETRY_DELAY_MS));
    }

    /* Empty RX blocks briefly; telemetry still gets a turn under continuous RX. */
    byte_received = xQueueReceive(s_serial_rx_queue, &received_byte,
                                   pdMS_TO_TICKS(SERIAL_RX_WAIT_MS));
    sample_received = (xQueueReceive(s_serial_sample_queue, &sample, 0U) == pdPASS) ? 1U : 0U;
    if (sample_received != 0U)
    {
      /* Update before handling a command, so GET uses the newest mailbox copy. */
      SerialCommand_UpdateSample(&sample, HAL_GetTick());
    }

    if (xQueueReceive(s_alarm_response_queue, &response, 0U) == pdPASS)
    {
      if (SerialCommand_HandleAlarmResponse(&response) != HAL_OK)
      {
        vTaskDelay(pdMS_TO_TICKS(SERIAL_RETRY_DELAY_MS));
      }
    }

    /* Check after the blocking receive, before accepting its byte. RX loss can
     * otherwise join unrelated fragments into an unintended valid command. */
    SerialPort_GetStats(&stats);
    if ((stats.rx_dropped_count != last_dropped_count) ||
        (stats.rx_error_count != last_error_count))
    {
      if (xQueueReset(s_serial_rx_queue) != pdPASS)
      {
        Error_Handler();
      }
      SerialCommand_DiscardLine(HAL_GetTick());
      last_dropped_count = stats.rx_dropped_count;
      last_error_count = stats.rx_error_count;
      byte_received = pdFAIL;
    }

    /* One click sends a command. A quiet RX interval ends the command;
     * explicit CR/LF still works. Poll only after an empty receive wait so
     * already-queued suffix bytes are consumed before command execution. */
    if (byte_received == pdPASS)
    {
      command_status = SerialCommand_ProcessByte(received_byte, HAL_GetTick());
    }
    else
    {
      command_status = SerialCommand_Poll(HAL_GetTick());
    }
    if (command_status != HAL_OK)
    {
      vTaskDelay(pdMS_TO_TICKS(SERIAL_RETRY_DELAY_MS));
    }

    /* Sampling and alarm decisions never wait for UART TX. A failed report is
     * dropped; the next acquisition supplies fresh data instead of old retries. */
    if (sample_received != 0U)
    {
      if ((ProjectCommands_TransferActive() == 0U) && (SerialReport_SendSample(&sample) != HAL_OK))
      {
        vTaskDelay(pdMS_TO_TICKS(SERIAL_RETRY_DELAY_MS));
      }
    }
  }
}


/** @brief Own external Flash and perform bounded, yielding storage work. */
static void StartStorageTask(void *argument)
{
  (void)argument;
  StorageService_Init(s_storage_request_queue, s_storage_sample_queue,
                       s_storage_boot_queue, s_storage_saved_queue, s_storage_text_queue);
  for (;;)
  {
    StorageService_Poll();
    vTaskDelay(pdMS_TO_TICKS(STORAGE_POLL_INTERVAL_MS));
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  if (SerialPort_Init() != HAL_OK)
  {
    Error_Handler();
  }
  /* PA9 stays GPIO-high until the enabled UART can provide its idle level. */
  MX_GPIO_USART1_TX_Init();
  (void)TimeService_Init();
  (void)Pot_Init();
  /* USER CODE END 2 */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* W25Q is owned by storageTask; no cross-device mutex is required. */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  s_env_sample_queue = xQueueCreate(LATEST_VALUE_QUEUE_LENGTH, sizeof(EnvAlarm_Sample));
  s_alarm_state_queue = xQueueCreate(LATEST_VALUE_QUEUE_LENGTH, sizeof(EnvAlarm_State));
  s_serial_rx_queue = xQueueCreate(SERIAL_RX_QUEUE_LENGTH, sizeof(uint8_t));
  s_serial_sample_queue = xQueueCreate(LATEST_VALUE_QUEUE_LENGTH, sizeof(EnvAlarm_Sample));
  s_alarm_request_queue = xQueueCreate(LATEST_VALUE_QUEUE_LENGTH, sizeof(EnvAlarm_Request));
  s_alarm_response_queue = xQueueCreate(LATEST_VALUE_QUEUE_LENGTH, sizeof(EnvAlarm_Response));
  s_storage_request_queue = xQueueCreate(STORAGE_REQUEST_QUEUE_LENGTH, sizeof(Storage_Request));
  s_storage_sample_queue = xQueueCreate(HISTORY_RECORD_QUEUE_LENGTH, sizeof(History_Sample));
  s_storage_boot_queue = xQueueCreate(LATEST_VALUE_QUEUE_LENGTH, sizeof(Storage_Boot));
  s_storage_saved_queue = xQueueCreate(LATEST_VALUE_QUEUE_LENGTH, sizeof(Storage_SaveReply));
  s_storage_text_queue = xQueueCreate(STORAGE_TEXT_QUEUE_LENGTH, sizeof(Storage_TextReply));
  if ((s_env_sample_queue == NULL) ||
      (s_alarm_state_queue == NULL) ||
      (s_serial_rx_queue == NULL) ||
      (s_serial_sample_queue == NULL) ||
      (s_alarm_request_queue == NULL) ||
      (s_alarm_response_queue == NULL) ||
      (s_storage_request_queue == NULL) ||
      (s_storage_sample_queue == NULL) ||
      (s_storage_boot_queue == NULL) ||
      (s_storage_saved_queue == NULL) ||
      (s_storage_text_queue == NULL))
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* USER CODE BEGIN RTOS_THREADS */
  /* FreeRTOS stack depth is in words: 256 words = 1024 bytes on Cortex-M3. */
  if (xTaskCreate(StartEnvMonitorTask,
                  "envMonitorTask",
                  ENV_MONITOR_TASK_STACK_WORDS,
                  NULL,
                  ENV_MONITOR_TASK_PRIORITY,
                  &s_env_monitor_task_handle) != pdPASS)
  {
    Error_Handler();
  }

  if (xTaskCreate(StartAlarmTask,
                  "alarmTask",
                  ALARM_TASK_STACK_WORDS,
                  NULL,
                  ALARM_TASK_PRIORITY,
                  &s_alarm_task_handle) != pdPASS)
  {
    Error_Handler();
  }
  if (xTaskCreate(StartSerialTask,
                  "serialTask",
                  SERIAL_TASK_STACK_WORDS,
                  NULL,
                  SERIAL_TASK_PRIORITY,
                  &s_serial_task_handle) != pdPASS)
  {
    Error_Handler();
  }
  
  if (xTaskCreate(StartStorageTask, "storageTask", STORAGE_TASK_STACK_WORDS,
                  NULL, STORAGE_TASK_PRIORITY, &s_storage_task_handle) != pdPASS)
  {
    Error_Handler();
  }
  RtosDiagnostics_Bind(s_env_monitor_task_handle, s_alarm_task_handle,
                        s_serial_task_handle, s_storage_task_handle);
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  vTaskStartScheduler();

  /* A successful scheduler start never returns. */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    Error_Handler();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC|RCC_PERIPHCLK_ADC;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV8;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM2 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM2) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
