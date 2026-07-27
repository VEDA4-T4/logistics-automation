#ifndef INPUT_MOTOR_H
#define INPUT_MOTOR_H

#include <stdint.h>

typedef enum { INPUT_MOTOR_OK = 0, INPUT_MOTOR_ERROR = 1 } input_motor_result_t;

typedef struct {
    void* context;
    input_motor_result_t (*initialize)(void* context);
    input_motor_result_t (*apply)(void* context, uint8_t running, uint8_t speed);
} input_motor_port_t;

#endif /* INPUT_MOTOR_H */
