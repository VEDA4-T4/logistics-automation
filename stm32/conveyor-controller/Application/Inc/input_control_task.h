#ifndef INPUT_CONTROL_TASK_H
#define INPUT_CONTROL_TASK_H

#include <stdint.h>

#include "app_messages.h"
#include "input_control.h"

void StartInputControlTask(void* argument);

/* CommTxTask can provide a strong implementation without changing this task. */
uint8_t input_control_task_publish_status(app_uart_channel_t destination, uint8_t sequence,
                                          const uint8_t payload[UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE]);

#endif /* INPUT_CONTROL_TASK_H */
