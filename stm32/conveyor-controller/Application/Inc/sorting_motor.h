#ifndef SORTING_MOTOR_H
#define SORTING_MOTOR_H

#include <stdint.h>

typedef enum { SORTING_MOTOR_OK = 0, SORTING_MOTOR_ERROR = 1 } sorting_motor_result_t;

typedef struct {
    void* context;
    sorting_motor_result_t (*initialize)(void* context);
    sorting_motor_result_t (*apply)(void* context, uint8_t running, uint8_t speed);
} sorting_motor_port_t;

#endif /* SORTING_MOTOR_H */
