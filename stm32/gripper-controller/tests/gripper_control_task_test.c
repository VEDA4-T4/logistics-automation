#include "gripper_control_task.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "gripper_servo.h"

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t gripperControlQueueHandle = (osMessageQueueId_t)1;
osMessageQueueId_t safetyCommandQueueHandle;
osMessageQueueId_t commTxQueueHandle;

static uint32_t fakeSafetyEpoch;
static uint32_t eventSendAttempts;
static int32_t eventSendResult;
static uint8_t lastEventPayload[UART_MAX_PAYLOAD_SIZE];
static uint8_t lastEventLength;

static int32_t fake_enable(void* context) {
    (void)context;
    return 0;
}

static int32_t fake_write_arm(void* context, uint16_t base_angle, uint16_t shoulder_angle, uint16_t elbow_angle) {
    (void)context;
    (void)base_angle;
    (void)shoulder_angle;
    (void)elbow_angle;
    return 0;
}

static int32_t fake_write_gripper(void* context, uint8_t position_percent) {
    (void)context;
    (void)position_percent;
    return 0;
}

static const gripper_servo_port_t fakeServoPort = {
    .context = NULL,
    .enable = fake_enable,
    .write_arm = fake_write_arm,
    .write_gripper = fake_write_gripper,
};

const gripper_servo_port_t* gripper_servo_mg90s_port(void) {
    return &fakeServoPort;
}

uint32_t safety_task_get_epoch(void) {
    return fakeSafetyEpoch;
}

uint8_t safety_task_is_latched(void) {
    return 0U;
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

uint32_t HAL_GetTick(void) {
    return 0U;
}

int32_t CommTx_Send(uint8_t command, const uint8_t* payload, uint8_t length) {
    assert(command == UART_CMD_EVENT);
    eventSendAttempts++;
    lastEventLength = length;
    memcpy(lastEventPayload, payload, length);
    return eventSendResult;
}

int32_t CommTx_SendWithSequence(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length) {
    (void)sequence;
    (void)command;
    (void)payload;
    (void)length;
    return 0;
}

int32_t CommTx_SendUrgent(uint8_t command, const uint8_t* payload, uint8_t length) {
    (void)command;
    (void)payload;
    (void)length;
    return 0;
}

int32_t CommTx_SendUrgentWithSequence(uint8_t sequence, uint8_t command, const uint8_t* payload, uint8_t length) {
    (void)sequence;
    (void)command;
    (void)payload;
    (void)length;
    return 0;
}

void CommTx_SetDeviceStatus(uint8_t state, uint8_t error) {
    (void)state;
    (void)error;
}

void CommTx_GetStats(comm_tx_stats_t* stats) {
    (void)stats;
}

#include "../Application/Src/gripper_control_task.c"

static void reset_task_state(void) {
    memset(&gripperTaskStats, 0, sizeof(gripperTaskStats));
    memset(&gripperCache, 0, sizeof(gripperCache));
    gripperPendingResponseValid = 0U;
    gripperPendingEventValid = 0U;
    atomic_store_explicit(&gripperSafetyPendingFlags, 0U, memory_order_release);
    assert(gripper_control_init(&gripperController, &fakeServoPort) == GRIPPER_CONTROL_OK);
}

static void test_stop_and_release_requests_are_both_applied(void) {
    reset_task_state();
    gripperController.homed = 1U;

    assert(gripper_control_task_notify_safety_stop() == 1U);
    assert(gripper_control_task_notify_safety_release() == 1U);
    gripper_control_task_apply_pending_safety();

    assert(gripperController.state == UART_GRIPPER_STATE_STOPPED);
    assert(gripperController.homed == 0U);
    assert(atomic_load_explicit(&gripperSafetyPendingFlags, memory_order_acquire) == 0U);
}

static void test_completion_event_is_retried_after_queue_failure(void) {
    reset_task_state();
    eventSendAttempts = 0U;
    eventSendResult = -1;
    gripperController.completion.valid = 1U;
    gripperController.completion.motion_id = 7U;
    gripperController.completion.motion_type = UART_GRIPPER_MOTION_ARM;

    gripper_control_task_emit_completion();
    assert(eventSendAttempts == 1U);
    assert(gripperPendingEventValid == 1U);

    eventSendResult = 0;
    assert(gripper_control_task_service_tx() == 1U);
    assert(eventSendAttempts == 2U);
    assert(gripperPendingEventValid == 0U);
    assert(lastEventLength == UART_GRIPPER_MOTION_EVENT_PAYLOAD_SIZE);
    assert(lastEventPayload[UART_EVENT_ID_INDEX] == UART_GRIPPER_EVENT_MOTION_COMPLETE);
    assert(lastEventPayload[UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX] == 7U);
}

int main(void) {
    test_stop_and_release_requests_are_both_applied();
    test_completion_event_is_retried_after_queue_failure();
    puts("gripper_control_task_test: PASS");
    return 0;
}
