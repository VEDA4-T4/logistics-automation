#include "app_queues.h"

#include <stddef.h>

#include "app_messages.h"

#define UART_RX_QUEUE_DEPTH 8U
#define GRIPPER_CONTROL_QUEUE_DEPTH 8U
#define SAFETY_COMMAND_QUEUE_DEPTH 4U
#define COMM_TX_QUEUE_DEPTH 12U

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t gripperControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;
osMessageQueueId_t commTxQueueHandle;

static const osMessageQueueAttr_t uartRxQueueAttributes = {.name = "uartRxQueue"};
static const osMessageQueueAttr_t gripperControlQueueAttributes = {.name = "gripperControlQueue"};
static const osMessageQueueAttr_t safetyCommandQueueAttributes = {.name = "safetyCommandQueue"};
static const osMessageQueueAttr_t commTxQueueAttributes = {.name = "commTxQueue"};

osStatus_t app_queues_init(void) {
    uartRxQueueHandle = osMessageQueueNew(UART_RX_QUEUE_DEPTH, sizeof(uart_rx_chunk_t), &uartRxQueueAttributes);
    gripperControlQueueHandle =
        osMessageQueueNew(GRIPPER_CONTROL_QUEUE_DEPTH, sizeof(control_command_t), &gripperControlQueueAttributes);
    safetyCommandQueueHandle =
        osMessageQueueNew(SAFETY_COMMAND_QUEUE_DEPTH, sizeof(control_command_t), &safetyCommandQueueAttributes);
    commTxQueueHandle = osMessageQueueNew(COMM_TX_QUEUE_DEPTH, sizeof(comm_tx_message_t), &commTxQueueAttributes);

    if (uartRxQueueHandle == NULL || gripperControlQueueHandle == NULL || safetyCommandQueueHandle == NULL ||
        commTxQueueHandle == NULL) {
        return osErrorNoMemory;
    }

    return osOK;
}
