#ifndef TEST_FAKES_TASK_H
#define TEST_FAKES_TASK_H

void test_task_enter_critical(void);
void test_task_exit_critical(void);

#define taskENTER_CRITICAL() test_task_enter_critical()
#define taskEXIT_CRITICAL() test_task_exit_critical()

#endif /* TEST_FAKES_TASK_H */
