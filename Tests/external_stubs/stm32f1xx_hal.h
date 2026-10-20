#ifndef EXTERNAL_TEST_HAL_H
#define EXTERNAL_TEST_HAL_H
#include <stdint.h>
#include <stddef.h>
typedef enum { HAL_OK=0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t Mode,Direction,DataSize,CLKPolarity,CLKPhase,NSS,BaudRatePrescaler,FirstBit,TIMode,CRCCalculation,CRCPolynomial; } SPI_InitTypeDef;
typedef struct { void *Instance; SPI_InitTypeDef Init; uint32_t ErrorCode; } SPI_HandleTypeDef;
typedef struct { uint32_t ScanConvMode,ContinuousConvMode,DiscontinuousConvMode,ExternalTrigConv,DataAlign,NbrOfConversion; } ADC_InitTypeDef;
typedef struct { void *Instance; ADC_InitTypeDef Init; } ADC_HandleTypeDef;
typedef struct { uint32_t Channel,Rank,SamplingTime; } ADC_ChannelConfTypeDef;
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef enum { GPIO_PIN_RESET=0, GPIO_PIN_SET=1 } GPIO_PinState;
#define SPI1 ((void *)1)
#define ADC1 ((void *)2)
#define GPIOB ((GPIO_TypeDef *)3)
#define SPI_MODE_MASTER 1U
#define SPI_DIRECTION_2LINES 2U
#define SPI_DATASIZE_8BIT 8U
#define SPI_POLARITY_LOW 0U
#define SPI_PHASE_1EDGE 0U
#define SPI_NSS_SOFT 1U
#define SPI_BAUDRATEPRESCALER_64 64U
#define SPI_FIRSTBIT_MSB 0U
#define SPI_TIMODE_DISABLE 0U
#define SPI_CRCCALCULATION_DISABLE 0U
#define ADC_SCAN_DISABLE 0U
#define DISABLE 0U
#define ADC_SOFTWARE_START 1U
#define ADC_DATAALIGN_RIGHT 0U
#define ADC_CHANNEL_0 0U
#define ADC_REGULAR_RANK_1 1U
#define ADC_SAMPLETIME_239CYCLES_5 239U
#define __HAL_RCC_SPI1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI1_FORCE_RESET() ((void)0)
#define __HAL_RCC_SPI1_RELEASE_RESET() ((void)0)
#define __HAL_RCC_ADC1_CLK_ENABLE() ((void)0)
uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *spi);
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *spi);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *spi,uint8_t *data,uint16_t length,uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *spi,uint8_t *tx,uint8_t *rx,uint16_t length,uint32_t timeout);
void HAL_GPIO_WritePin(GPIO_TypeDef *port,uint16_t pin,GPIO_PinState state);
HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *adc);
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *adc,ADC_ChannelConfTypeDef *channel);
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *adc);
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *adc);
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *adc);
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *adc,uint32_t timeout);
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *adc);
#endif
