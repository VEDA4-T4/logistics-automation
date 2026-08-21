#include "safety_task.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "gripper_control_task.h"

#define GPIO_PIN_13 13U

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t gripperControlQueueHandle = (osMessageQueueId_t)1;
osMessageQueueId_t safetyCommandQueueHandle = (osMessageQueueId_t)1;
osMessageQueueId_t commTxQueueHandle;

static gripper_control_snapshot_t fakeSnapshot;
static uint32_t stopNotifications;
static uint32_t releaseNotifications;
static uint32_t eventAttempts;
static int32_t eventResult;
static uint8_t lastEvent[3];
static uint8_t lastDeviceState;
static uint8_t lastDeviceError;

uint8_t gripper_control_task_notify_safety_stop(void) {
    stopNotifications++;
    return 1U;
}

uint8_t gripper_control_task_notify_safety_release(void) {
    releaseNotifications++;
    return 1U;
}

void gripper_control_task_get_snapshot(gripper_control_snapshot_t* snapshot) {
    *snapshot = fakeSnapshot;
}

void gripper_control_task_get_stats(gripper_control_task_stats_t* stats) {
    (void)stats;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t queue, const void* message, uint8_t priority, uint32_t timeout) {
    (void)message;
    (void)priority;
    (void)timeout;
    return (queue != NULL) ? osOK : osError;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t queue, void* message, uint8_t* priority, uint32_t timeout) {
    (void)queue;
    (void)message;
    (void)priority;
    (void)timeout;
    return osError;
}

void osDelay(uint32_t ticks) {
    (void)ticks;
}

int32_t CommTx_SendUrgent(uint8_t command, const uint8_t* payload, uint8_t length) {
    assert(command == UART_CMD_EVENT);
    assert(length == sizeof(lastEvent));
    eventAttempts++;
    memcpy(lastEvent, payload, length);
    return eventResult;
}

void CommTx_SetDeviceStatus(uint8_t state, uint8_t error) {
    lastDeviceState = state;
    lastDeviceError = error;
}

#include "../Application/Src/safety_task.c"

static control_command_t safety_command(app_control_message_kind_t kind, uint8_t command) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.kind = kind;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.command = command;
    return message;
}

static void reset_fakes(void) {
    memset(&fakeSnapshot, 0, sizeof(fakeSnapshot));
    fakeSnapshot.state = UART_GRIPPER_STATE_STOPPED;
    stopNotifications = 0U;
    releaseNotifications = 0U;
    eventAttempts = 0U;
    eventResult = 0;
    memset(lastEvent, 0, sizeof(lastEvent));
    lastDeviceState = UART_DEVICE_IDLE;
    lastDeviceError = UART_ERROR_NONE;
    SafetyTask_Init();
}

static void test_recovery_is_idempotent(void) {
    const control_command_t reset = safety_command(APP_CONTROL_MESSAGE_SAFETY_RELEASE, UART_CMD_RESET_DEVICE);

    reset_fakes();
    SafetyTask_HandleSafetyCommand(&reset);
    SafetyTask_HandleSafetyCommand(&reset);

    assert(eventAttempts == 2U);
    assert(lastEvent[0] == APP_EVENT_SAFETY);
    assert(lastEvent[1] == 0U);
    assert(lastEvent[2] == UART_CMD_RESET_DEVICE);
    assert(lastDeviceState == UART_DEVICE_STOPPED);
    assert(lastDeviceError == UART_ERROR_NONE);
    assert(releaseNotifications == 0U);
}

static void test_release_waits_for_control_task(void) {
    const control_command_t stop = safety_command(APP_CONTROL_MESSAGE_SAFETY_STOP, UART_CMD_EMERGENCY_STOP);
    const control_command_t reset = safety_command(APP_CONTROL_MESSAGE_SAFETY_RELEASE, UART_CMD_RESET_DEVICE);
    safety_task_stats_t stats;

    reset_fakes();
    SafetyTask_HandleSafetyCommand(&stop);
    fakeSnapshot.state = UART_GRIPPER_STATE_EMERGENCY_STOP;
    fakeSnapshot.safety_latched = 1U;
    SafetyTask_HandleSafetyCommand(&reset);

    assert(safety_task_is_latched() != 0U);
    assert(releaseNotifications == 1U);
    assert(eventAttempts == 1U);

    SafetyTask_ServicePending();
    assert(eventAttempts == 1U);

    fakeSnapshot.state = UART_GRIPPER_STATE_STOPPED;
    fakeSnapshot.safety_latched = 0U;
    SafetyTask_ServicePending();

    assert(safety_task_is_latched() == 0U);
    assert(eventAttempts == 2U);
    assert(lastEvent[1] == 0U);
    safety_task_get_stats(&stats);
    assert(stats.releases == 1U);
}

static void test_failed_event_enqueue_is_retried(void) {
    const control_command_t reset = safety_command(APP_CONTROL_MESSAGE_SAFETY_RELEASE, UART_CMD_RESET_DEVICE);

    reset_fakes();
    eventResult = -1;
    SafetyTask_HandleSafetyCommand(&reset);
    assert(eventAttempts == 1U);
    assert(safetyEventPending != 0U);

    eventResult = 0;
    SafetyTask_ServicePending();
    assert(eventAttempts == 2U);
    assert(safetyEventPending == 0U);
}

static void test_prestart_local_estop_is_preserved(void) {
    reset_fakes();
    assert(safety_task_request_local_estop() == 1U);

    SafetyTask_Init();
    SafetyTask_ServicePending();

    assert(stopNotifications == 1U);
    assert(safety_task_is_latched() != 0U);
}

int main(void) {
    test_recovery_is_idempotent();
    test_release_waits_for_control_task();
    test_failed_event_enqueue_is_retried();
    test_prestart_local_estop_is_preserved();
    puts("gripper_safety_task_test: PASS");
    return 0;
}
