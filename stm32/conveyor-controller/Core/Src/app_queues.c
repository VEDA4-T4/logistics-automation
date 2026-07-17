#include "app_queues.h"

#include "app_messages.h"

#define UART_RX_QUEUE_DEPTH          8U
#define INPUT_CONTROL_QUEUE_DEPTH    8U
#define SORTING_CONTROL_QUEUE_DEPTH  8U
#define SAFETY_COMMAND_QUEUE_DEPTH   4U

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t inputControlQueueHandle;
osMessageQueueId_t sortingControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;

static const osMessageQueueAttr_t uartRxQueueAttributes =
{
    .name = "uartRxQueue"
};

static const osMessageQueueAttr_t inputControlQueueAttributes =
{
    .name = "inputControlQueue"
};

static const osMessageQueueAttr_t sortingControlQueueAttributes =
{
    .name = "sortingControlQueue"
};

static const osMessageQueueAttr_t safetyCommandQueueAttributes =
{
    .name = "safetyCommandQueue"
};

osStatus_t app_queues_init(void)
{
    uartRxQueueHandle =
        osMessageQueueNew(
            UART_RX_QUEUE_DEPTH,
            sizeof(uart_rx_chunk_t),
            &uartRxQueueAttributes);

    inputControlQueueHandle =
        osMessageQueueNew(
            INPUT_CONTROL_QUEUE_DEPTH,
            sizeof(control_command_t),
            &inputControlQueueAttributes);

    sortingControlQueueHandle =
        osMessageQueueNew(
            SORTING_CONTROL_QUEUE_DEPTH,
            sizeof(control_command_t),
            &sortingControlQueueAttributes);

    safetyCommandQueueHandle =
        osMessageQueueNew(
            SAFETY_COMMAND_QUEUE_DEPTH,
            sizeof(control_command_t),
            &safetyCommandQueueAttributes);

    if ((uartRxQueueHandle == NULL) ||
        (inputControlQueueHandle == NULL) ||
        (sortingControlQueueHandle == NULL) ||
        (safetyCommandQueueHandle == NULL))
    {
        return osErrorNoMemory;
    }

    return osOK;
}
