/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_queues.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for CommRxTask */
osThreadId_t CommRxTaskHandle;
const osThreadAttr_t CommRxTask_attributes = {
  .name = "CommRxTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for GripperControlTask */
osThreadId_t GripperControlTaskHandle;
const osThreadAttr_t GripperControlTask_attributes = {
  .name = "GripperControlTask",
  .stack_size = 768 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for SafetyTask */
osThreadId_t SafetyTaskHandle;
const osThreadAttr_t SafetyTask_attributes = {
  .name = "SafetyTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for CommTxTask */
osThreadId_t CommTxTaskHandle;
const osThreadAttr_t CommTxTask_attributes = {
  .name = "CommTxTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for HealthTask */
osThreadId_t HealthTaskHandle;
const osThreadAttr_t HealthTask_attributes = {
  .name = "HealthTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartCommRxTask(void *argument);
void StartGripperControlTask(void *argument);
void StartSafetyTask(void *argument);
void StartCommTxTask(void *argument);
void StartHealthTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  if (app_queues_init() != osOK)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of CommRxTask */
  CommRxTaskHandle = osThreadNew(StartCommRxTask, NULL, &CommRxTask_attributes);

  /* creation of GripperControlTask */
  GripperControlTaskHandle = osThreadNew(StartGripperControlTask, NULL, &GripperControlTask_attributes);

  /* creation of SafetyTask */
  SafetyTaskHandle = osThreadNew(StartSafetyTask, NULL, &SafetyTask_attributes);

  /* creation of CommTxTask */
  CommTxTaskHandle = osThreadNew(StartCommTxTask, NULL, &CommTxTask_attributes);

  /* creation of HealthTask */
  HealthTaskHandle = osThreadNew(StartHealthTask, NULL, &HealthTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartCommRxTask */
/**
  * @brief  Function implementing the CommRxTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartCommRxTask */
__weak void StartCommRxTask(void *argument)
{
  /* USER CODE BEGIN StartCommRxTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartCommRxTask */
}

/* USER CODE BEGIN Header_StartGripperControlTask */
/**
* @brief Function implementing the GripperControlTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartGripperControlTask */
__weak void StartGripperControlTask(void *argument)
{
  /* USER CODE BEGIN StartGripperControlTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartGripperControlTask */
}

/* USER CODE BEGIN Header_StartSafetyTask */
/**
* @brief Function implementing the SafetyTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSafetyTask */
__weak void StartSafetyTask(void *argument)
{
  /* USER CODE BEGIN StartSafetyTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartSafetyTask */
}

/* USER CODE BEGIN Header_StartCommTxTask */
/**
* @brief Function implementing the CommTxTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartCommTxTask */
__weak void StartCommTxTask(void *argument)
{
  /* USER CODE BEGIN StartCommTxTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartCommTxTask */
}

/* USER CODE BEGIN Header_StartHealthTask */
/**
* @brief Function implementing the HealthTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartHealthTask */
__weak void StartHealthTask(void *argument)
{
  /* USER CODE BEGIN StartHealthTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartHealthTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

