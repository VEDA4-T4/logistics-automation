#ifndef INPUT_MOTOR_TEST_MAIN_H
#define INPUT_MOTOR_TEST_MAIN_H

#include <stdint.h>

typedef struct {
    uint32_t unused;
} GPIO_TypeDef;

typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

/* fakes/stm32f4xx_hal.h와 동시에 include될 수 있어(app_comm_tx.h 경유) 공유 guard로 중복 정의를 막는다. */
#ifndef TEST_FAKES_HAL_STATUS_TYPEDEF
#define TEST_FAKES_HAL_STATUS_TYPEDEF
typedef enum { HAL_OK = 0x00U, HAL_ERROR = 0x01U, HAL_BUSY = 0x02U } HAL_StatusTypeDef;
#endif

extern GPIO_TypeDef inputMotorTestGpioB;
extern GPIO_TypeDef sortingMotorTestGpioA;
extern GPIO_TypeDef sensorTestTrigGpioC;

#define INPUT_MOTOR_AIN1_GPIO_Port (&inputMotorTestGpioB)
#define INPUT_MOTOR_AIN1_Pin (1U << 5U)
#define INPUT_MOTOR_AIN2_GPIO_Port (&inputMotorTestGpioB)
#define INPUT_MOTOR_AIN2_Pin (1U << 4U)

#define SORTING_MOTOR_BIN1_GPIO_Port (&sortingMotorTestGpioA)
#define SORTING_MOTOR_BIN1_Pin (1U << 7U)
#define SORTING_MOTOR_BIN2_GPIO_Port (&sortingMotorTestGpioA)
#define SORTING_MOTOR_BIN2_Pin (1U << 6U)

#define US1_TRIG_GPIO_Port (&sensorTestTrigGpioC)
#define US1_TRIG_Pin (1U << 2U)
#define US2_TRIG_GPIO_Port (&sensorTestTrigGpioC)
#define US2_TRIG_Pin (1U << 3U)
#define US3_TRIG_GPIO_Port (&sensorTestTrigGpioC)
#define US3_TRIG_Pin (1U << 4U)
#define US4_TRIG_GPIO_Port (&sensorTestTrigGpioC)
#define US4_TRIG_Pin (1U << 5U)

void HAL_GPIO_WritePin(GPIO_TypeDef* gpioPort, uint16_t gpioPin, GPIO_PinState pinState);
uint32_t HAL_GetTick(void);

#endif /* INPUT_MOTOR_TEST_MAIN_H */
