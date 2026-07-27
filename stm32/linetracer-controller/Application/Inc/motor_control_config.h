#ifndef MOTOR_CONTROL_CONFIG_H
#define MOTOR_CONTROL_CONFIG_H

#define MOTOR_CONTROL_PWM_MAX 999U
#define MOTOR_CONTROL_BASE_PWM 600
#define MOTOR_CONTROL_CORRECTION_PWM 250
#define MOTOR_CONTROL_PIVOT_PWM 500
#define MOTOR_CONTROL_LEFT_TRIM 0
#define MOTOR_CONTROL_RIGHT_TRIM 0

/* Set to 1 when a motor is wired opposite to the logical forward direction. */
#define MOTOR_CONTROL_LEFT_REVERSED 0U
#define MOTOR_CONTROL_RIGHT_REVERSED 0U

#if MOTOR_CONTROL_BASE_PWM < 0 || MOTOR_CONTROL_BASE_PWM > MOTOR_CONTROL_PWM_MAX
#error "Motor base PWM must fit the TIM3 period"
#endif

#if MOTOR_CONTROL_CORRECTION_PWM < 0
#error "Motor correction PWM must not be negative"
#endif

#if MOTOR_CONTROL_PIVOT_PWM < 0 || MOTOR_CONTROL_PIVOT_PWM > MOTOR_CONTROL_PWM_MAX
#error "Motor pivot PWM must fit the TIM3 period"
#endif

#if MOTOR_CONTROL_LEFT_REVERSED > 1U || MOTOR_CONTROL_RIGHT_REVERSED > 1U
#error "Motor reverse flags must be 0 or 1"
#endif

#endif /* MOTOR_CONTROL_CONFIG_H */
