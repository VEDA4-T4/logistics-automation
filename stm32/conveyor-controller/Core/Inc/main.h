/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define B1_Pin GPIO_PIN_13
#define B1_GPIO_Port GPIOC
#define US1_TRIG_Pin GPIO_PIN_2
#define US1_TRIG_GPIO_Port GPIOC
#define US2_TRIG_Pin GPIO_PIN_3
#define US2_TRIG_GPIO_Port GPIOC
#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA
#define SORTING_MOTOR_BIN2_Pin GPIO_PIN_6
#define SORTING_MOTOR_BIN2_GPIO_Port GPIOA
#define SORTING_MOTOR_BIN1_Pin GPIO_PIN_7
#define SORTING_MOTOR_BIN1_GPIO_Port GPIOA
#define US3_TRIG_Pin GPIO_PIN_4
#define US3_TRIG_GPIO_Port GPIOC
#define US4_TRIG_Pin GPIO_PIN_5
#define US4_TRIG_GPIO_Port GPIOC
#define INPUT_MOTOR_PWMA_Pin GPIO_PIN_8
#define INPUT_MOTOR_PWMA_GPIO_Port GPIOA
#define TMS_Pin GPIO_PIN_13
#define TMS_GPIO_Port GPIOA
#define TCK_Pin GPIO_PIN_14
#define TCK_GPIO_Port GPIOA
#define SWO_Pin GPIO_PIN_3
#define SWO_GPIO_Port GPIOB
#define INPUT_MOTOR_AIN2_Pin GPIO_PIN_4
#define INPUT_MOTOR_AIN2_GPIO_Port GPIOB
#define INPUT_MOTOR_AIN1_Pin GPIO_PIN_5
#define INPUT_MOTOR_AIN1_GPIO_Port GPIOB
#define MOTOR_STBY_Pin GPIO_PIN_6
#define MOTOR_STBY_GPIO_Port GPIOB
#define SORTING_MOTOR_PWMB_Pin GPIO_PIN_9
#define SORTING_MOTOR_PWMB_GPIO_Port GPIOB
#define SORTING_GATE_PWM_Pin GPIO_PIN_7
#define SORTING_GATE_PWM_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
