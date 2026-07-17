#include "input_control_task.h"

#include "app_queues.h"
#include "cmsis_os2.h"
#include "input_motor_tb6612.h"
#include "main.h"

#define INPUT_CONTROL_QUEUE_RETRY_TICKS 100U

static input_control_t inputController;

__weak uint8_t input_control_task_publish_status(app_uart_channel_t destination, uint8_t sequence,
                                                 const uint8_t payload[UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE]) {
    (void)destination;
    (void)sequence;
    (void)payload;
    return 0U;
}

void StartInputControlTask(void* argument) {
    control_command_t message;
    input_control_result_t result;
    uint8_t statusPayload[UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE];

    (void)argument;

    (void)input_control_init(&inputController, input_motor_tb6612_port());

    for (;;) {
        if (inputControlQueueHandle == NULL) {
            osDelay(INPUT_CONTROL_QUEUE_RETRY_TICKS);
            continue;
        }

        if (osMessageQueueGet(inputControlQueueHandle, &message, NULL, osWaitForever) != osOK) {
            continue;
        }

        result = input_control_process_command(&inputController, &message);

        if ((result == INPUT_CONTROL_OK) && (message.frame.command == UART_CMD_INPUT_CONVEYOR_GET_STATUS)) {
            input_control_build_status_payload(&inputController.state, statusPayload);

            (void)input_control_task_publish_status(message.source, message.frame.sequence, statusPayload);
        }
    }
}
