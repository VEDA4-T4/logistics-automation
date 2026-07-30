#include "health_task.h"

#include "app_comm_tx.h"
#include "cmsis_os2.h"
#include "gripper_control_task.h"
#include "safety_task.h"

#define HEALTH_POLL_PERIOD_MS 250U

void StartHealthTask(void* argument) {
    gripper_control_snapshot_t snapshot;

    (void)argument;
    for (;;) {
        gripper_control_task_get_snapshot(&snapshot);
        if (safety_task_is_latched() != 0U) {
            CommTx_SetDeviceStatus(UART_DEVICE_EMERGENCY_STOP, UART_ERROR_EMERGENCY_STOP);
        } else if (snapshot.state == UART_GRIPPER_STATE_FAULT) {
            CommTx_SetDeviceStatus(UART_DEVICE_ERROR, UART_ERROR_SERVO);
        } else if (snapshot.state == UART_GRIPPER_STATE_MOVING_ARM ||
                   snapshot.state == UART_GRIPPER_STATE_MOVING_GRIPPER ||
                   snapshot.state == UART_GRIPPER_STATE_HOMING) {
            CommTx_SetDeviceStatus(UART_DEVICE_RUNNING, UART_ERROR_NONE);
        } else if (snapshot.state == UART_GRIPPER_STATE_STOPPED) {
            CommTx_SetDeviceStatus(UART_DEVICE_STOPPED, UART_ERROR_NONE);
        } else {
            CommTx_SetDeviceStatus(UART_DEVICE_READY, UART_ERROR_NONE);
        }
        osDelay(HEALTH_POLL_PERIOD_MS);
    }
}
