#ifndef SORTING_GATE_H
#define SORTING_GATE_H

#include <stdint.h>

#include "logistics/contracts/uart/sorting_commands.h"

typedef enum { SORTING_GATE_OK = 0, SORTING_GATE_ERROR = 1 } sorting_gate_result_t;

typedef struct {
    void* context;
    sorting_gate_result_t (*initialize)(void* context);
    sorting_gate_result_t (*move)(void* context, uart_sorting_destination_t destination);
    sorting_gate_result_t (*motion_complete)(void* context, uint8_t* complete);
} sorting_gate_port_t;

#endif /* SORTING_GATE_H */
