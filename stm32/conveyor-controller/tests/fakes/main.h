#ifndef INPUT_MOTOR_TEST_MAIN_H
#define INPUT_MOTOR_TEST_MAIN_H

#include <stdint.h>

typedef struct {
    uint32_t unused;
} GPIO_TypeDef;

typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;

extern GPIO_TypeDef inputMotorTestGpioB;
extern GPIO_TypeDef sortingMotorTestGpioA;

#define INPUT_MOTOR_AIN1_GPIO_Port (&inputMotorTestGpioB)
#define INPUT_MOTOR_AIN1_Pin (1U << 5U)
#define INPUT_MOTOR_AIN2_GPIO_Port (&inputMotorTestGpioB)
#define INPUT_MOTOR_AIN2_Pin (1U << 4U)

#define SORTING_MOTOR_BIN1_GPIO_Port (&sortingMotorTestGpioA)
#define SORTING_MOTOR_BIN1_Pin (1U << 7U)
#define SORTING_MOTOR_BIN2_GPIO_Port (&sortingMotorTestGpioA)
#define SORTING_MOTOR_BIN2_Pin (1U << 6U)

void HAL_GPIO_WritePin(GPIO_TypeDef* gpioPort, uint16_t gpioPin, GPIO_PinState pinState);
uint32_t HAL_GetTick(void);

#endif /* INPUT_MOTOR_TEST_MAIN_H */
