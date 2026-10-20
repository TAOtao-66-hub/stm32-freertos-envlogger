/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.h
  * @brief   This file contains all the function prototypes for
  *          the gpio.c file
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
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __GPIO_H__
#define __GPIO_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN Private defines */

/* Pin ownership: configuration and initial levels are listed in gpio.c. */
#define BUZZER_Pin        GPIO_PIN_2
#define BUZZER_GPIO_Port  GPIOA
#define BUTTON_Pin        GPIO_PIN_3
#define BUTTON_GPIO_Port  GPIOA
#define LED_Pin           GPIO_PIN_4
#define LED_GPIO_Port     GPIOA
#define SERIAL_TX_Pin       GPIO_PIN_9
#define SERIAL_TX_GPIO_Port GPIOA
#define SERIAL_RX_Pin       GPIO_PIN_10
#define SERIAL_RX_GPIO_Port GPIOA

#define FLASH_CS_Pin        GPIO_PIN_0
#define FLASH_CS_GPIO_Port  GPIOB
#define FLASH_SCK_Pin       GPIO_PIN_5
#define FLASH_MISO_Pin      GPIO_PIN_6
#define FLASH_MOSI_Pin      GPIO_PIN_7
#define POT_ADC_Pin         GPIO_PIN_0

/* USER CODE END Private defines */

/* Configure pin modes and initial levels. See the pin table in gpio.c. */
void MX_GPIO_Init(void);

/* USER CODE BEGIN Prototypes */

/* Complete the TX handover only after SerialPort_Init succeeds. */
void MX_GPIO_USART1_TX_Init(void);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif
#endif /* __GPIO_H__ */

