#ifndef TEST_FAKES_TASK_H
#define TEST_FAKES_TASK_H

#include "FreeRTOS.h"

void test_task_enter_critical(void);
void test_task_exit_critical(void);

#define taskENTER_CRITICAL() test_task_enter_critical()
#define taskEXIT_CRITICAL() test_task_exit_critical()

/*
 * CommTxTask는 urgent/normal 두 송신 큐를 태스크 알림으로 기다린다
 * (app_comm_tx.c 참고). 실제 FreeRTOS에서 xTaskNotifyGive는
 * xTaskGenericNotify로 펼쳐지는 매크로지만, 테스트에서는 알림 횟수만
 * 세면 되므로 여기서는 함수로 두어 테스트가 직접 구현하게 한다.
 */
typedef void* TaskHandle_t;

TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);

#endif /* TEST_FAKES_TASK_H */
