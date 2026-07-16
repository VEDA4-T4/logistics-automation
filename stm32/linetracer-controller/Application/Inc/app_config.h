#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "main.h"

/*
 * 이 파일만 수정하면 되는 조정값 모음.
 * PWM 값은 0~APP_PWM_MAX 범위이며 기본 설정에서는 0~999이다.
 */

#define APP_UART_BAUDRATE              115200U
#define APP_COMMAND_BUFFER_SIZE        64U
#define APP_COMMAND_QUEUE_DEPTH        4U

#define APP_PWM_MAX                    999U
#define APP_EXPECTED_TIM3_PRESCALER    83U      /* dev.ioc가 소유하는 값, 현재 약 1 kHz */
#define APP_DEFAULT_BASE_PWM           600U
#define APP_DEFAULT_CORRECTION_PWM     250U
#define APP_DEFAULT_LEFT_TRIM          0
#define APP_DEFAULT_RIGHT_TRIM         0
#define APP_DIRECTION_DEADTIME_MS      2U

#define APP_LINE_SENSOR_ACTIVE_LOW     1U       /* TCRT5000 모듈에 맞춰 0 또는 1 */
#define APP_LINE_SAMPLE_PERIOD_MS      5U
#define APP_LINE_DEBOUNCE_SAMPLES      3U
#define APP_LINE_LOST_TIMEOUT_MS       1000U

#define APP_LEFT_MOTOR_REVERSED        0U       /* 실제 바퀴 회전이 반대면 1 */
#define APP_RIGHT_MOTOR_REVERSED       0U

#define APP_RAMP_STEP                  100U
#define APP_RAMP_INTERVAL_MS           400U
#define APP_TEST_MANUAL_PWM            500U
#define APP_TEST_SLOW_PWM              300U
#define APP_TEST_FAST_PWM              700U

/* 실제 Raspberry Pi heartbeat 규격이 정해진 뒤 1로 바꾼다. */
#define APP_COMM_WATCHDOG_ENABLE       0U
#define APP_COMM_WATCHDOG_TIMEOUT_MS   2000U

/*
 * dev.ioc에서 아직 User Label을 지정하지 않은 핀의 별칭이다.
 * 나중에 CubeMX에서 같은 이름으로 Label을 지정해도 중복 정의되지 않는다.
 */
#ifndef MOTOR_L_IN1_Pin
#define MOTOR_L_IN1_Pin                GPIO_PIN_0
#define MOTOR_L_IN1_GPIO_Port          GPIOC
#endif
#ifndef MOTOR_L_IN2_Pin
#define MOTOR_L_IN2_Pin                GPIO_PIN_1
#define MOTOR_L_IN2_GPIO_Port          GPIOC
#endif
#ifndef MOTOR_R_IN1_Pin
#define MOTOR_R_IN1_Pin                GPIO_PIN_2
#define MOTOR_R_IN1_GPIO_Port          GPIOC
#endif
#ifndef MOTOR_R_IN2_Pin
#define MOTOR_R_IN2_Pin                GPIO_PIN_3
#define MOTOR_R_IN2_GPIO_Port          GPIOC
#endif
#ifndef MOTOR_STBY_Pin
#define MOTOR_STBY_Pin                 GPIO_PIN_4
#define MOTOR_STBY_GPIO_Port           GPIOC
#endif

#ifndef LINE_L_Pin
#define LINE_L_Pin                     GPIO_PIN_4
#define LINE_L_GPIO_Port               GPIOB
#endif
#ifndef LINE_R_Pin
#define LINE_R_Pin                     GPIO_PIN_5
#define LINE_R_GPIO_Port               GPIOB
#endif

#ifndef ESTOP_Pin
#define ESTOP_Pin                      GPIO_PIN_12
#define ESTOP_GPIO_Port                GPIOB
#endif

#endif
