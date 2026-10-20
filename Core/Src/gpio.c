/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/*
 * 引脚配置总表：以下为 MX_GPIO_Init() 执行后的状态，并非芯片刚复位时的状态。
 *
 * 引脚   用途                 GPIO 模式       上下拉       初始电平 / 有效电平
 * PA0    B5K 电位器滑动端     模拟输入        无           0~3.3V，由旋钮决定
 * PA2    有源蜂鸣器控制       推挽输出        无           高：关闭；低：触发
 * PA3    消音按键输入         输入            内部上拉     高：松开；低：按下
 * PA4    报警 LED 控制        推挽输出        无           低：熄灭；高：点亮
 * PA5    W25Q CLK / SPI1 SCK   复用推挽        无           SPI 模式 0，空闲低
 * PA6    W25Q DO / SPI1 MISO   浮空输入        无           未选中时无固定电平
 * PA7    W25Q DI / SPI1 MOSI   复用推挽        无           预置低，空闲电平不作协议条件
 * PA9    USART1 TX            推挽 -> 复用推挽 无          先 GPIO 主动拉高；USART1 就绪后接管
 * PA10   USART1 RX            输入            内部上拉     空闲高；接 USB-TTL 的 TXD
 * PB0    W25Q CS              推挽输出        无           高：未选中；低：选中
 * PB6    OLED SCL             开漏输出        外部上拉     写 1 释放，上拉后高
 * PB7    OLED SDA             开漏输出        外部上拉     写 1 释放，上拉后高
 * PB10   DHT11 DAT            输入            内部上拉     空闲高
 *
 * 接线：PA3--按键--GND；PA4--330 ohm 至 1 kohm--LED 正极，LED 负极接 GND。
 * PA2 接低电平触发蜂鸣器模块的 I/O，实际供电电压须按模块规格确认。
 * 串口接线：PA9--USB-TTL RXD，PA10--USB-TTL TXD，GND--GND，信号电平为 3.3V。
 * PA9 分两步初始化：MX_GPIO_Init 先推挽拉高，SerialPort_Init 成功后，
 * main 调用 MX_GPIO_USART1_TX_Init 切换复用，避免先复用、后使能外设的空窗。
 * 复位期间 PA9 仍是高阻输入；本表不保证复位期间的电平。
 * PB6/PB7 需要上拉到 3.3V；开漏输出写 1 表示释放，不是主动输出高电平。
 * PB10 在 dht11.c 中按协议切换：启动时开漏拉低，接收时切换为 EXTI 输入。
 * PA13/PA14 保留给 SWD 下载调试，本文件不重新配置。
 * 引脚宏在 gpio.h 中定义；更换引脚时同步更新本总表和 .ioc 硬件参考配置。
 * LED、蜂鸣器与按键由 alarmTask 独占操作，初始模式统一在本文件配置。
 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * @brief  Configure initial GPIO modes and levels for the environment monitor.
  * @note   Write output levels before enabling output drivers.
  */
void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* 1. GPIO 时钟：本工程使用 GPIOA 和 GPIOB。 */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* 2. 预写输出电平，避免切换到输出模式时出现不期望的脉冲。 */
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(SERIAL_TX_GPIO_Port, SERIAL_TX_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);

  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, FLASH_SCK_Pin | FLASH_MOSI_Pin, GPIO_PIN_RESET);

  /* PA0: 5k linear potentiometer, fixed terminals 3.3V/GND, wiper PA0. */
  GPIO_InitStruct.Pin = POT_ADC_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* PB0 software CS is high before enabling SPI alternate outputs. */
  GPIO_InitStruct.Pin = FLASH_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(FLASH_CS_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = FLASH_SCK_Pin | FLASH_MOSI_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = FLASH_MISO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* 3. PA2：有源蜂鸣器，推挽输出，初始高电平（关闭）。 */
  GPIO_InitStruct.Pin = BUZZER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(BUZZER_GPIO_Port, &GPIO_InitStruct);

  /* 4. PA3：消音按键，上拉输入；按键另一端接 GND，按下读到低电平。 */
  GPIO_InitStruct.Pin = BUTTON_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BUTTON_GPIO_Port, &GPIO_InitStruct);

  /* 5. PA4：报警 LED，推挽输出，初始低电平（熄灭）。 */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /* 6. PA9：先用普通推挽输出高，USART1 就绪后再连接复用输出。 */
  GPIO_InitStruct.Pin = SERIAL_TX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(SERIAL_TX_GPIO_Port, &GPIO_InitStruct);

  /* 7. PA10：USART1 RX，上拉输入；空闲高，避免未接串口模块时悬空。 */
  GPIO_InitStruct.Pin = SERIAL_RX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(SERIAL_RX_GPIO_Port, &GPIO_InitStruct);

  /* 8. PB6/PB7：OLED 软件 I2C，开漏输出，初始释放为高；需要外部上拉。 */
  GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* 9. PB10：DHT11 DAT，初始上拉输入；通信时的模式切换由驱动负责。 */
  GPIO_InitStruct.Pin = GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/* USER CODE BEGIN 2 */

/**
  * @brief  Connect PA9 to USART1 only after the UART transmitter is enabled.
  * @note   main calls this once after SerialPort_Init returns HAL_OK.
  *         ODR preloading alone cannot hold an alternate-function output high.
  */
void MX_GPIO_USART1_TX_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Pin = SERIAL_TX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(SERIAL_TX_GPIO_Port, &GPIO_InitStruct);
}

/* USER CODE END 2 */
