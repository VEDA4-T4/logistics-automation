#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "input_control_task.h"
#include "safety_task.h"
#include "sorting_control_task.h"
#include "sorting_gate_mg90s.h"
#include "sorting_motor_tb6612.h"

/*
 * ============================================================================
 * SafetyTask <-> InputControlTask/SortingControlTask 연동 테스트
 * ============================================================================
 *
 * safety_task_test.c는 InputControl/SortingControl을 fake로 대체해 SafetyTask
 * 자체 로직만 검증한다. 이 파일은 그 둘의 **실제 구현**(input_control.c/
 * input_control_task.c, sorting_control.c/sorting_control_task.c)을 그대로
 * 링크해 SafetyTask가 실제 상태머신과 맞물려 끝까지 동작하는지 검증한다.
 * 모터/게이트 물리 드라이버와 conveyor_motor_power(GPIO/인터럽트 직접 접근)는
 * 여전히 fake로 대체한다 — 이 경계 밖은 대상이 아니다.
 */

osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t inputControlQueueHandle;
osMessageQueueId_t sortingControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;

#define FAKE_QUEUE_MAX_ITEMS 16U

typedef struct {
    size_t itemSize;
    uint32_t count;
    uint8_t items[FAKE_QUEUE_MAX_ITEMS][sizeof(control_command_t)];
} fake_queue_t;

static fake_queue_t inputQueue;
static fake_queue_t sortingQueue;

static void fake_queue_reset(fake_queue_t* queue, size_t itemSize) {
    memset(queue, 0, sizeof(*queue));
    queue->itemSize = itemSize;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void* msg_ptr, uint8_t msg_prio, uint32_t timeout) {
    fake_queue_t* queue;

    (void)msg_prio;
    (void)timeout;

    if ((mq_id == NULL) || (msg_ptr == NULL)) {
        return osErrorParameter;
    }

    queue = (fake_queue_t*)mq_id;
    if (queue->count >= FAKE_QUEUE_MAX_ITEMS) {
        return osErrorResource;
    }

    memcpy(queue->items[queue->count], msg_ptr, queue->itemSize);
    queue->count++;
    return osOK;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void* msg_ptr, uint8_t* msg_prio, uint32_t timeout) {
    fake_queue_t* queue;

    (void)msg_prio;
    (void)timeout;

    if ((mq_id == NULL) || (msg_ptr == NULL)) {
        return osErrorParameter;
    }

    queue = (fake_queue_t*)mq_id;
    if (queue->count == 0U) {
        return osErrorResource;
    }

    memcpy(msg_ptr, queue->items[0], queue->itemSize);
    queue->count--;
    if (queue->count != 0U) {
        memmove(queue->items[0], queue->items[1], queue->count * sizeof(queue->items[0]));
    }
    return osOK;
}

osStatus_t osDelay(uint32_t ticks) {
    (void)ticks;
    return osOK;
}

/* ---- fake: 공통 모터 전원(STBY latch). GPIO/인터럽트락은 대상 밖. ---- */
static uint32_t latchDisableCalls;
static uint32_t releaseLatchCalls;
static uint8_t motorPowerLatched;

uint8_t conveyor_motor_power_enable(void) {
    return (motorPowerLatched == 0U) ? 1U : 0U;
}

void conveyor_motor_power_latch_disable(void) {
    latchDisableCalls++;
    motorPowerLatched = 1U;
}

void conveyor_motor_power_release_latch(void) {
    releaseLatchCalls++;
    motorPowerLatched = 0U;
}

/* ---- fake: 투입 모터(TB6612) ---- */
typedef struct {
    uint32_t initializeCalls;
    uint32_t applyCalls;
    uint8_t running;
    uint8_t speed;
} fake_input_motor_t;

static fake_input_motor_t inputMotor;
static input_motor_port_t inputMotorPort;

static input_motor_result_t fake_input_motor_initialize(void* context) {
    ((fake_input_motor_t*)context)->initializeCalls++;
    return INPUT_MOTOR_OK;
}

static input_motor_result_t fake_input_motor_apply(void* context, uint8_t running, uint8_t speed) {
    fake_input_motor_t* motor = (fake_input_motor_t*)context;
    motor->applyCalls++;
    motor->running = running;
    motor->speed = speed;
    return INPUT_MOTOR_OK;
}

const input_motor_port_t* input_motor_tb6612_port(void) {
    return &inputMotorPort;
}

/* ---- fake: 분류 모터(TB6612) + 게이트(MG90S) ---- */
typedef struct {
    uint32_t initializeCalls;
    uint32_t applyCalls;
} fake_sorting_motor_t;

typedef struct {
    uint32_t initializeCalls;
    uint32_t moveCalls;
    uint8_t motionComplete; /* 테스트가 직접 조작: 게이트가 물리적으로 다 움직였는지 */
} fake_sorting_gate_t;

static fake_sorting_motor_t sortingMotor;
static fake_sorting_gate_t sortingGate;
static sorting_motor_port_t sortingMotorPort;
static sorting_gate_port_t sortingGatePort;

static sorting_motor_result_t fake_sorting_motor_initialize(void* context) {
    ((fake_sorting_motor_t*)context)->initializeCalls++;
    return SORTING_MOTOR_OK;
}

static sorting_motor_result_t fake_sorting_motor_apply(void* context, uint8_t running, uint8_t speed) {
    (void)running;
    (void)speed;
    ((fake_sorting_motor_t*)context)->applyCalls++;
    return SORTING_MOTOR_OK;
}

static sorting_gate_result_t fake_sorting_gate_initialize(void* context) {
    fake_sorting_gate_t* gate = (fake_sorting_gate_t*)context;
    gate->initializeCalls++;
    gate->motionComplete = 1U; /* 초기화 직후 이미 Home으로 간주 */
    return SORTING_GATE_OK;
}

static sorting_gate_result_t fake_sorting_gate_move(void* context, uart_sorting_destination_t destination) {
    (void)destination;
    fake_sorting_gate_t* gate = (fake_sorting_gate_t*)context;
    gate->moveCalls++;
    gate->motionComplete = 0U; /* 이동 시작: 아직 완료 아님 */
    return SORTING_GATE_OK;
}

static sorting_gate_result_t fake_sorting_gate_motion_complete(void* context, uint8_t* complete) {
    fake_sorting_gate_t* gate = (fake_sorting_gate_t*)context;
    *complete = gate->motionComplete;
    return SORTING_GATE_OK;
}

const sorting_motor_port_t* sorting_motor_tb6612_port(void) {
    return &sortingMotorPort;
}

const sorting_gate_port_t* sorting_gate_mg90s_port(void) {
    return &sortingGatePort;
}

/* ---- fake: CommTx ---- */
static uint32_t setDeviceStatusCalls;
static uint8_t lastDeviceState;

void CommTx_SetDeviceStatus(uint8_t device_state, uint8_t error_code) {
    (void)error_code;
    setDeviceStatusCalls++;
    lastDeviceState = device_state;
}

int32_t CommTx_Send(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    (void)channel;
    (void)command;
    (void)payload;
    (void)length;
    return 0;
}

int32_t CommTx_SendWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command, const uint8_t* payload,
                                uint8_t length) {
    (void)channel;
    (void)sequence;
    (void)command;
    (void)payload;
    (void)length;
    return 0;
}

int32_t CommTx_SendUrgent(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    (void)channel;
    (void)command;
    (void)payload;
    (void)length;
    return 0;
}

/* ---- fake: HAL ---- */
uint32_t HAL_GetTick(void) {
    return 0U;
}

/* ---- 테스트 헬퍼 ---- */
static input_control_t inputController;
static sorting_control_t sortingController;

/* StartInputControlTask/StartSortingControlTask의 루프가 만드는 것과 동일한 안전 마커. */
static void drive_input_marker(app_control_message_kind_t kind) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = APP_UART_CHANNEL_1;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.kind = kind;
    (void)input_control_task_process_message(&inputController, &message);
}

static void drive_sorting_marker(app_control_message_kind_t kind) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = APP_UART_CHANNEL_6;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.kind = kind;
    (void)sorting_control_task_process_message(&sortingController, &message);
}

static control_command_t safety_command(app_uart_channel_t source, uint8_t command) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = source;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.command = command;
    message.kind = APP_CONTROL_MESSAGE_UART_COMMAND;
    return message;
}

static void reset_all(void) {
    fake_queue_reset(&inputQueue, sizeof(control_command_t));
    fake_queue_reset(&sortingQueue, sizeof(control_command_t));
    uartRxQueueHandle = NULL;
    inputControlQueueHandle = &inputQueue;
    sortingControlQueueHandle = &sortingQueue;
    safetyCommandQueueHandle = NULL;

    latchDisableCalls = 0U;
    releaseLatchCalls = 0U;
    motorPowerLatched = 0U;

    setDeviceStatusCalls = 0U;
    lastDeviceState = 0U;

    memset(&inputMotor, 0, sizeof(inputMotor));
    inputMotorPort.context = &inputMotor;
    inputMotorPort.initialize = fake_input_motor_initialize;
    inputMotorPort.apply = fake_input_motor_apply;

    memset(&sortingMotor, 0, sizeof(sortingMotor));
    sortingMotorPort.context = &sortingMotor;
    sortingMotorPort.initialize = fake_sorting_motor_initialize;
    sortingMotorPort.apply = fake_sorting_motor_apply;

    memset(&sortingGate, 0, sizeof(sortingGate));
    sortingGatePort.context = &sortingGate;
    sortingGatePort.initialize = fake_sorting_gate_initialize;
    sortingGatePort.move = fake_sorting_gate_move;
    sortingGatePort.motion_complete = fake_sorting_gate_motion_complete;

    assert(input_control_task_initialize_controller(&inputController, &inputMotorPort) == INPUT_CONTROL_OK);
    assert(sorting_control_task_initialize_controller(&sortingController, &sortingMotorPort, &sortingGatePort) ==
           SORTING_CONTROL_OK);
    assert(sortingGate.motionComplete == 1U); /* 초기화 직후 게이트는 Home */

    SafetyTask_Init();
}

/*
 * 전체 흐름: 분류 Pi에서 E-Stop 수신 -> 두 공정 정지 통지 -> 분류 게이트가
 * 아직 물리적으로 이동 중이라 STOPPED 지연 -> 게이트 도착 -> 두 공정 STOPPED
 * 확인 -> 투입 Pi에서 Reset -> 두 공정 RELEASED 확인 후에야 STBY latch 해제.
 */
static void test_full_estop_and_reset_cycle_with_real_control_tasks(void) {
    reset_all();

    /* 사전 조건: 두 공정 모두 정상 동작 상태(RELEASED). */
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_RELEASED);

    /* 분류 게이트가 임의로 이동 중이라고 가정(실제 운용 중 발생 가능한 시나리오). */
    sortingGate.motionComplete = 0U;

    /* 1) 분류 Pi에서 E-Stop 수신. */
    control_command_t estop = safety_command(APP_UART_CHANNEL_6, UART_CMD_EMERGENCY_STOP);
    SafetyTask_HandleSafetyCommand(&estop);

    /* STBY latch는 즉시, 두 공정 다 STOP_REQUESTED로 즉시 전이(atomic). */
    assert(latchDisableCalls == 1U);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOP_REQUESTED);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_STOP_REQUESTED);
    assert(setDeviceStatusCalls == 1U);
    assert(lastDeviceState == UART_DEVICE_EMERGENCY_STOP);

    /* 2) 각 ControlTask 루프가 하듯 안전 마커를 처리시킨다. */
    drive_input_marker(APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_STOPPED);
    assert(inputMotor.applyCalls >= 1U); /* 실제로 모터에 정지가 적용됨 */

    /* 분류는 게이트가 아직 Home이 아니라 STOPPED로 전이하지 않는다(MOTION_PENDING). */
    drive_sorting_marker(APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_STOP_REQUESTED);

    /* 3) SafetyTask가 이 시점에 reset을 시도해도 아직 해제를 요청하면 안 된다. */
    control_command_t reset = safety_command(APP_UART_CHANNEL_1, UART_CMD_RESET_DEVICE);
    SafetyTask_HandleSafetyCommand(&reset);
    SafetyTask_ServicePending();
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_STOP_REQUESTED);
    assert(releaseLatchCalls == 0U);

    /* 4) 게이트가 물리적으로 Home에 도착. */
    sortingGate.motionComplete = 1U;
    drive_sorting_marker(APP_CONTROL_MESSAGE_SAFETY_STOP);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_STOPPED);

    /* 5) 이제 SafetyTask가 두 공정 모두 STOPPED임을 확인하고 해제를 요청한다. */
    SafetyTask_ServicePending();
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASE_REQUESTED);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_RELEASE_REQUESTED);
    assert(releaseLatchCalls == 0U); /* 해제 요청만으로는 latch를 풀지 않는다 */

    /* 6) 각 ControlTask가 해제 마커를 처리해 RELEASED로 전이. */
    drive_input_marker(APP_CONTROL_MESSAGE_SAFETY_RELEASE);
    assert(input_control_task_get_safety_sync_state() == INPUT_CONTROL_SAFETY_RELEASED);

    drive_sorting_marker(APP_CONTROL_MESSAGE_SAFETY_RELEASE);
    assert(sorting_control_task_get_safety_sync_state() == SORTING_CONTROL_SAFETY_RELEASED);

    /* 7) 두 공정 모두 RELEASED 확인 후에야 SafetyTask가 STBY latch를 푼다. */
    SafetyTask_ServicePending();
    assert(releaseLatchCalls == 1U);
    assert(SafetyTask_IsReleasing() == 0U);
    assert(lastDeviceState == UART_DEVICE_READY);
    assert(conveyor_motor_power_enable() == 1U); /* latch가 실제로 풀려 다시 enable 가능 */
}

int main(void) {
    test_full_estop_and_reset_cycle_with_real_control_tasks();
    return 0;
}
