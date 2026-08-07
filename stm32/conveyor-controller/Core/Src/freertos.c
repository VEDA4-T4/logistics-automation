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
#include "fault_trap.h"
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
/* Definitions for InputControlTask */
osThreadId_t InputControlTaskHandle;
const osThreadAttr_t InputControlTask_attributes = {
  .name = "InputControlTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for SortingControlTask */
osThreadId_t SortingControlTaskHandle;
const osThreadAttr_t SortingControlTask_attributes = {
  .name = "SortingControlTask",
  .stack_size = 512 * 4,
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
/* Definitions for SensorTask */
osThreadId_t SensorTaskHandle;
const osThreadAttr_t SensorTask_attributes = {
  .name = "SensorTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
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
void StartInputControlTask(void *argument);
void StartSortingControlTask(void *argument);
void StartSafetyTask(void *argument);
void StartCommTxTask(void *argument);
void StartSensorTask(void *argument);
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
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of CommRxTask */
  CommRxTaskHandle = osThreadNew(StartCommRxTask, NULL, &CommRxTask_attributes);

  /* creation of InputControlTask */
  InputControlTaskHandle = osThreadNew(StartInputControlTask, NULL, &InputControlTask_attributes);

  /* creation of SortingControlTask */
  SortingControlTaskHandle = osThreadNew(StartSortingControlTask, NULL, &SortingControlTask_attributes);

  /* creation of SafetyTask */
  SafetyTaskHandle = osThreadNew(StartSafetyTask, NULL, &SafetyTask_attributes);

  /* creation of CommTxTask */
  CommTxTaskHandle = osThreadNew(StartCommTxTask, NULL, &CommTxTask_attributes);

  /* creation of SensorTask */
  SensorTaskHandle = osThreadNew(StartSensorTask, NULL, &SensorTask_attributes);

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
* @brief Function implementing the CommRxTask thread.
* @param argument: Not used
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

/* USER CODE BEGIN Header_StartInputControlTask */
/**
* @brief Function implementing the InputControlTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartInputControlTask */
__weak void StartInputControlTask(void *argument)
{
  /* USER CODE BEGIN StartInputControlTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartInputControlTask */
}

/* USER CODE BEGIN Header_StartSortingControlTask */
/**
* @brief Function implementing the SortingControlTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSortingControlTask */
__weak void StartSortingControlTask(void *argument)
{
  /* USER CODE BEGIN StartSortingControlTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartSortingControlTask */
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

/* USER CODE BEGIN Header_StartSensorTask */
/**
* @brief Function implementing the SensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSensorTask */
__weak void StartSensorTask(void *argument)
{
  /* USER CODE BEGIN StartSensorTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartSensorTask */
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

/*
 * 스택 오버플로 훅(configCHECK_FOR_STACK_OVERFLOW=2).
 *
 * FreeRTOS는 이 훅에서 돌아오는 것을 기대하지 않는다. 어느 태스크가 넘쳤는지
 * .noinit에 남기고 멈추면, IWDG가 리셋한 뒤에도 태스크 이름이 남는다.
 * 검사가 꺼져 있던 동안에는 넘친 태스크가 남의 메모리를 조용히 망가뜨리다
 * 한참 뒤 엉뚱한 곳에서 HardFault로 터졌을 것이다.
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;

  FaultTrap_CaptureTask(FAULT_TRAP_STACK_OVERFLOW, pcTaskName);

  taskDISABLE_INTERRUPTS();
  for (;;)
  {
  }
}

/* 힙 할당 실패 훅(configUSE_MALLOC_FAILED_HOOK=1). 같은 이유로 기록 후 정지한다.
 * 태스크 이름은 남기지 않는다 - pcTaskGetName()은 INCLUDE_pcTaskGetTaskName이
 * 필요한데, 그 정의는 CubeMX가 관리하는 영역이라 .ioc 재생성 때 사라지면
 * 링크가 깨진다. 힙 고갈은 대개 초기화 중에 나므로 종류만으로도 충분하다. */
void vApplicationMallocFailedHook(void)
{
  FaultTrap_CaptureTask(FAULT_TRAP_MALLOC_FAILED, NULL);

  taskDISABLE_INTERRUPTS();
  for (;;)
  {
  }
}

/* USER CODE END Application */

