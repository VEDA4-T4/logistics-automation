#ifdef NDEBUG
#undef NDEBUG
#endif

#include "safety_task.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_comm_tx.h"
#include "app_queues.h"
#include "conveyor_motor_power.h"
#include "health_task.h"
#include "input_control_task.h"
#include "sorting_control_task.h"

void Health_TaskAlive(health_task_id_t id) {
    (void)id;
}

/* ---- 안전 큐 핸들(safety_task.c가 extern으로 참조) ---- */
osMessageQueueId_t uartRxQueueHandle;
osMessageQueueId_t inputControlQueueHandle;
osMessageQueueId_t sortingControlQueueHandle;
osMessageQueueId_t safetyCommandQueueHandle;

/* ---- 관찰용 전역 상태 ---- */
static uint32_t callSeq;

static uint32_t latchDisableCalls;
static uint32_t latchDisableSeq;
static uint32_t releaseLatchCalls;
static uint32_t releaseLatchSeq;

static uint32_t inputNotifyStopCalls;
static uint32_t inputNotifyStopSeq;
static uint8_t inputNotifyStopResult;
static uint32_t inputNotifyReleaseCalls;
static int inputSync;

static uint32_t sortingNotifyStopCalls;
static uint32_t sortingNotifyStopSeq;
static uint8_t sortingNotifyStopResult;
static uint32_t sortingNotifyReleaseCalls;
static sorting_control_safety_sync_state_t sortingSync;

static uint32_t setDeviceStatusCalls;
static uint8_t lastDeviceState;
static uint8_t lastDeviceError;
static uint32_t setChannelStatusCalls[COMM_TX_CH_COUNT];
static uint8_t channelDeviceStates[COMM_TX_CH_COUNT];
static uint8_t channelDeviceErrors[COMM_TX_CH_COUNT];

typedef struct {
    comm_tx_channel_t channel;
    uint8_t command;
    uint8_t length;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
} tx_urgent_record_t;

#define TX_RECORD_MAX 16U
static tx_urgent_record_t txRecords[TX_RECORD_MAX];
static uint32_t txRecordCount;
static int sendUrgentFail;

static uint32_t fakeTick;

/* ---- fake: 공통 모터 전원(STBY latch) ---- */
uint8_t conveyor_motor_power_enable(void) {
    return 1U;
}

void conveyor_motor_power_latch_disable(void) {
    latchDisableCalls++;
    latchDisableSeq = ++callSeq;
}

void conveyor_motor_power_release_latch(void) {
    releaseLatchCalls++;
    releaseLatchSeq = ++callSeq;
}

/* ---- fake: InputControlTask 안전 seam ---- */
uint8_t input_control_task_notify_safety_stop(void) {
    inputNotifyStopCalls++;
    inputNotifyStopSeq = ++callSeq;
    inputSync = (int)INPUT_CONTROL_SAFETY_STOP_REQUESTED;
    return inputNotifyStopResult;
}

uint8_t input_control_task_notify_safety_release(void) {
    inputNotifyReleaseCalls++;
    if (inputSync == (int)INPUT_CONTROL_SAFETY_STOPPED) {
        inputSync = (int)INPUT_CONTROL_SAFETY_RELEASE_REQUESTED;
        return 1U;
    }
    return 0U;
}

input_control_safety_sync_state_t input_control_task_get_safety_sync_state(void) {
    return (input_control_safety_sync_state_t)inputSync;
}

/* ---- fake: SortingControlTask 안전 seam(실제 sorting_control_task.h 시그니처) ---- */
uint8_t sorting_control_task_notify_safety_stop(void) {
    sortingNotifyStopCalls++;
    sortingNotifyStopSeq = ++callSeq;
    sortingSync = SORTING_CONTROL_SAFETY_STOP_REQUESTED;
    return sortingNotifyStopResult;
}

uint8_t sorting_control_task_notify_safety_release(void) {
    sortingNotifyReleaseCalls++;
    if (sortingSync == SORTING_CONTROL_SAFETY_STOPPED) {
        sortingSync = SORTING_CONTROL_SAFETY_RELEASE_REQUESTED;
        return 1U;
    }
    return 0U;
}

sorting_control_safety_sync_state_t sorting_control_task_get_safety_sync_state(void) {
    return sortingSync;
}

/* ---- fake: CommTx ---- */
void CommTx_SetDeviceStatus(uint8_t device_state, uint8_t error_code) {
    uint32_t i;

    setDeviceStatusCalls++;
    lastDeviceState = device_state;
    lastDeviceError = error_code;

    for (i = 0U; i < COMM_TX_CH_COUNT; i++) {
        channelDeviceStates[i] = device_state;
        channelDeviceErrors[i] = error_code;
    }
}

void CommTx_SetChannelDeviceStatus(comm_tx_channel_t channel, uint8_t device_state, uint8_t error_code) {
    assert(channel < COMM_TX_CH_COUNT);
    setChannelStatusCalls[channel]++;
    channelDeviceStates[channel] = device_state;
    channelDeviceErrors[channel] = error_code;
}

int32_t CommTx_SendUrgent(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length) {
    tx_urgent_record_t* record;

    if (sendUrgentFail != 0) {
        return -1;
    }

    if (txRecordCount < TX_RECORD_MAX) {
        record = &txRecords[txRecordCount];
        record->channel = channel;
        record->command = command;
        record->length = length;
        if ((length != 0U) && (payload != NULL)) {
            memcpy(record->payload, payload, length);
        }
    }
    txRecordCount++;
    return 0;
}

/* ---- fake: HAL / RTOS (StartSafetyTask 링크용) ---- */
uint32_t HAL_GetTick(void) {
    return fakeTick;
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void* msg_ptr, uint8_t* msg_prio, uint32_t timeout) {
    (void)mq_id;
    (void)msg_ptr;
    (void)msg_prio;
    (void)timeout;
    return osErrorResource;
}

/* ---- 테스트 헬퍼 ---- */
static void reset_all(void) {
    callSeq = 0U;
    latchDisableCalls = 0U;
    latchDisableSeq = 0U;
    releaseLatchCalls = 0U;
    releaseLatchSeq = 0U;
    inputNotifyStopCalls = 0U;
    inputNotifyStopSeq = 0U;
    inputNotifyStopResult = 1U;
    inputNotifyReleaseCalls = 0U;
    inputSync = (int)INPUT_CONTROL_SAFETY_RELEASED;
    sortingNotifyStopCalls = 0U;
    sortingNotifyStopSeq = 0U;
    sortingNotifyStopResult = 1U;
    sortingNotifyReleaseCalls = 0U;
    sortingSync = SORTING_CONTROL_SAFETY_RELEASED;
    setDeviceStatusCalls = 0U;
    lastDeviceState = 0U;
    lastDeviceError = 0U;
    memset(setChannelStatusCalls, 0, sizeof(setChannelStatusCalls));
    memset(channelDeviceStates, 0, sizeof(channelDeviceStates));
    memset(channelDeviceErrors, 0, sizeof(channelDeviceErrors));
    memset(txRecords, 0, sizeof(txRecords));
    txRecordCount = 0U;
    sendUrgentFail = 0;
    fakeTick = 0U;

    SafetyTask_Init();
}

static control_command_t make_command(app_uart_channel_t source, uint8_t command) {
    control_command_t message;

    memset(&message, 0, sizeof(message));
    message.source = source;
    message.frame.version = UART_PROTOCOL_VERSION;
    message.frame.command = command;
    message.kind = APP_CONTROL_MESSAGE_UART_COMMAND;
    return message;
}

static const tx_urgent_record_t* find_event(comm_tx_channel_t channel) {
    uint32_t i;

    for (i = 0U; (i < txRecordCount) && (i < TX_RECORD_MAX); i++) {
        if ((txRecords[i].channel == channel) && (txRecords[i].command == UART_CMD_EVENT)) {
            return &txRecords[i];
        }
    }
    return NULL;
}

static void drive_estop(app_uart_channel_t source) {
    control_command_t message = make_command(source, UART_CMD_EMERGENCY_STOP);
    SafetyTask_HandleSafetyCommand(&message);
}

static void drive_reset(app_uart_channel_t source) {
    control_command_t message = make_command(source, UART_CMD_RESET_DEVICE);
    SafetyTask_HandleSafetyCommand(&message);
}

/* ---- 테스트 케이스 ---- */

static void test_estop_latches_before_notifying_controls(void) {
    reset_all();
    fakeTick = 0x11223344U;

    drive_estop(APP_UART_CHANNEL_1);

    /* STBY latch가 공정 통지보다 먼저 일어나야 한다. */
    assert(latchDisableCalls == 1U);
    assert(inputNotifyStopCalls == 1U);
    assert(sortingNotifyStopCalls == 1U);
    assert(latchDisableSeq < inputNotifyStopSeq);
    assert(latchDisableSeq < sortingNotifyStopSeq);

    /* 장치 상태가 EMERGENCY_STOP으로 게시된다. */
    assert(setDeviceStatusCalls == 1U);
    assert(lastDeviceState == UART_DEVICE_EMERGENCY_STOP);
    assert(lastDeviceError == UART_ERROR_EMERGENCY_STOP);

    /* 양쪽 채널에 안전 EVENT가 보고된다. */
    assert(txRecordCount == 2U);
    const tx_urgent_record_t* inputEvent = find_event(COMM_TX_CH_INPUT);
    const tx_urgent_record_t* sortingEvent = find_event(COMM_TX_CH_SORTING);
    assert(inputEvent != NULL);
    assert(sortingEvent != NULL);
    assert(inputEvent->length == APP_SAFETY_EVENT_PAYLOAD_SIZE);
    assert(inputEvent->payload[UART_EVENT_ID_INDEX] == APP_EVENT_SAFETY);
    assert(inputEvent->payload[APP_SAFETY_EVENT_KIND_INDEX] == SAFETY_EVENT_ESTOP_LATCHED);
    assert(inputEvent->payload[APP_SAFETY_EVENT_CAUSE_INDEX] == SAFETY_CAUSE_ESTOP_INPUT_PI);
    assert(inputEvent->payload[APP_SAFETY_EVENT_RESULT_INDEX] == 0U);

    /* 타임스탬프(LE uint32)가 이벤트 발생 tick과 일치한다. */
    uint32_t reported = (uint32_t)inputEvent->payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX] |
                        ((uint32_t)inputEvent->payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 1U] << 8U) |
                        ((uint32_t)inputEvent->payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 2U] << 16U) |
                        ((uint32_t)inputEvent->payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 3U] << 24U);
    assert(reported == 0x11223344U);
}

static void test_estop_cause_maps_from_source_channel(void) {
    reset_all();
    drive_estop(APP_UART_CHANNEL_6);

    const tx_urgent_record_t* event = find_event(COMM_TX_CH_INPUT);
    assert(event != NULL);
    assert(event->payload[APP_SAFETY_EVENT_CAUSE_INDEX] == SAFETY_CAUSE_ESTOP_SORTING_PI);
}

static void test_stop_queue_failure_is_counted(void) {
    safety_task_stats_t stats;

    reset_all();
    inputNotifyStopResult = 0U;
    sortingNotifyStopResult = 0U;

    drive_estop(APP_UART_CHANNEL_1);

    Safety_GetStats(&stats);
    assert(stats.controlStopFailures == 2U);
    assert(stats.estopEvents == 1U);
}

static void test_input_reset_releases_only_input(void) {
    safety_task_stats_t stats;

    reset_all();
    drive_estop(APP_UART_CHANNEL_1);
    assert(SafetyTask_IsReleasing() == 0U);

    /* USART1 reset은 Input 공정의 해제만 시작한다. */
    drive_reset(APP_UART_CHANNEL_1);
    assert(SafetyTask_IsReleasing() == 1U);

    SafetyTask_ServicePending();
    assert(inputNotifyReleaseCalls == 0U); /* STOPPED 전에는 해제 요청 안 함 */
    assert(sortingNotifyReleaseCalls == 0U);
    assert(releaseLatchCalls == 0U);

    /* Sorting 상태와 무관하게 Input이 STOPPED이면 Input에만 해제를 요청한다. */
    inputSync = (int)INPUT_CONTROL_SAFETY_STOPPED;
    SafetyTask_ServicePending();
    assert(inputNotifyReleaseCalls == 1U);
    assert(sortingNotifyReleaseCalls == 0U);
    assert(releaseLatchCalls == 0U); /* 아직 RELEASED 확인 전 */
    assert(inputSync == (int)INPUT_CONTROL_SAFETY_RELEASE_REQUESTED);
    assert(sortingSync == SORTING_CONTROL_SAFETY_STOP_REQUESTED);

    /* Input RELEASED 확인 후 Input 채널만 READY가 된다. */
    inputSync = (int)INPUT_CONTROL_SAFETY_RELEASED;
    SafetyTask_ServicePending();
    assert(releaseLatchCalls == 1U);
    assert(SafetyTask_IsReleasing() == 0U);
    assert(setChannelStatusCalls[COMM_TX_CH_INPUT] == 1U);
    assert(setChannelStatusCalls[COMM_TX_CH_SORTING] == 0U);
    assert(channelDeviceStates[COMM_TX_CH_INPUT] == UART_DEVICE_READY);
    assert(channelDeviceErrors[COMM_TX_CH_INPUT] == UART_ERROR_NONE);
    assert(channelDeviceStates[COMM_TX_CH_SORTING] == UART_DEVICE_EMERGENCY_STOP);
    assert(channelDeviceErrors[COMM_TX_CH_SORTING] == UART_ERROR_EMERGENCY_STOP);

    /* E-Stop broadcast 2개 뒤 Reset 완료는 요청한 Input 채널에만 보고한다. */
    assert(txRecordCount == 3U);
    assert(txRecords[txRecordCount - 1U].channel == COMM_TX_CH_INPUT);
    assert(txRecords[txRecordCount - 1U].payload[APP_SAFETY_EVENT_KIND_INDEX] == SAFETY_EVENT_RESET_COMPLETE);
    assert(txRecords[txRecordCount - 1U].payload[APP_SAFETY_EVENT_RESULT_INDEX] == SAFETY_RESET_OK);

    Safety_GetStats(&stats);
    assert(stats.resetCompleted == 1U);
}

static void test_sorting_reset_releases_only_sorting(void) {
    reset_all();
    drive_estop(APP_UART_CHANNEL_6);
    drive_reset(APP_UART_CHANNEL_6);

    sortingSync = SORTING_CONTROL_SAFETY_STOPPED;
    SafetyTask_ServicePending();
    assert(inputNotifyReleaseCalls == 0U);
    assert(sortingNotifyReleaseCalls == 1U);
    assert(inputSync == (int)INPUT_CONTROL_SAFETY_STOP_REQUESTED);

    sortingSync = SORTING_CONTROL_SAFETY_RELEASED;
    SafetyTask_ServicePending();
    assert(setChannelStatusCalls[COMM_TX_CH_INPUT] == 0U);
    assert(setChannelStatusCalls[COMM_TX_CH_SORTING] == 1U);
    assert(channelDeviceStates[COMM_TX_CH_INPUT] == UART_DEVICE_EMERGENCY_STOP);
    assert(channelDeviceStates[COMM_TX_CH_SORTING] == UART_DEVICE_READY);
    assert(txRecords[txRecordCount - 1U].channel == COMM_TX_CH_SORTING);
}

static void test_both_scoped_resets_can_progress_independently(void) {
    reset_all();
    drive_estop(APP_UART_CHANNEL_1);
    drive_reset(APP_UART_CHANNEL_1);
    drive_reset(APP_UART_CHANNEL_6);

    inputSync = (int)INPUT_CONTROL_SAFETY_STOPPED;
    sortingSync = SORTING_CONTROL_SAFETY_STOPPED;
    SafetyTask_ServicePending();
    assert(inputNotifyReleaseCalls == 1U);
    assert(sortingNotifyReleaseCalls == 1U);

    inputSync = (int)INPUT_CONTROL_SAFETY_RELEASED;
    SafetyTask_ServicePending();
    assert(setChannelStatusCalls[COMM_TX_CH_INPUT] == 1U);
    assert(setChannelStatusCalls[COMM_TX_CH_SORTING] == 0U);
    assert(SafetyTask_IsReleasing() == 1U);

    sortingSync = SORTING_CONTROL_SAFETY_RELEASED;
    SafetyTask_ServicePending();
    assert(setChannelStatusCalls[COMM_TX_CH_SORTING] == 1U);
    assert(SafetyTask_IsReleasing() == 0U);
}

static void test_reset_times_out_and_keeps_latched(void) {
    safety_task_stats_t stats;
    uint32_t i;

    reset_all();
    drive_estop(APP_UART_CHANNEL_1);
    drive_reset(APP_UART_CHANNEL_1);

    /* ControlTask가 영영 STOPPED에 도달하지 않음 -> 예산 초과로 거부. */
    for (i = 0U; i < 300U; i++) {
        SafetyTask_ServicePending();
    }

    assert(releaseLatchCalls == 0U); /* latch 유지(fail-safe) */
    assert(SafetyTask_IsReleasing() == 0U);

    Safety_GetStats(&stats);
    assert(stats.resetRejected == 1U);
    assert(stats.resetCompleted == 0U);

    const tx_urgent_record_t* event = find_event(COMM_TX_CH_INPUT);
    assert(event != NULL);
    /* 마지막 이벤트가 RESET_REJECTED / INPUT_NOT_READY. */
    assert(txRecords[txRecordCount - 1U].payload[APP_SAFETY_EVENT_KIND_INDEX] == SAFETY_EVENT_RESET_REJECTED);
    assert(txRecords[txRecordCount - 1U].payload[APP_SAFETY_EVENT_RESULT_INDEX] == SAFETY_RESET_INPUT_NOT_READY);
}

static void test_estop_during_release_relatches(void) {
    reset_all();
    drive_estop(APP_UART_CHANNEL_1);
    drive_reset(APP_UART_CHANNEL_1);
    inputSync = (int)INPUT_CONTROL_SAFETY_STOPPED;
    sortingSync = SORTING_CONTROL_SAFETY_STOPPED;
    SafetyTask_ServicePending(); /* WAIT_RELEASED로 진입 */
    assert(SafetyTask_IsReleasing() == 1U);

    uint32_t latchesBefore = latchDisableCalls;
    drive_estop(APP_UART_CHANNEL_6); /* 해제 도중 새 E-Stop */

    assert(latchDisableCalls == latchesBefore + 1U);
    assert(SafetyTask_IsReleasing() == 0U); /* 해제 중단 */
    assert(releaseLatchCalls == 0U);
}

static void test_reset_without_estop_is_ignored(void) {
    safety_task_stats_t stats;

    reset_all();
    drive_reset(APP_UART_CHANNEL_1);

    assert(latchDisableCalls == 0U);
    assert(releaseLatchCalls == 0U);
    assert(txRecordCount == 0U);

    Safety_GetStats(&stats);
    assert(stats.resetIgnored == 1U);
}

static void test_fatal_trigger_enters_estop(void) {
    safety_task_stats_t stats;

    reset_all();
    Safety_TriggerEmergencyStop(SAFETY_CAUSE_FATAL_ERROR);

    /* 트리거는 비블로킹 플래그이므로 아직 차단 전. */
    assert(latchDisableCalls == 0U);

    SafetyTask_ServicePending();
    assert(latchDisableCalls == 1U);

    const tx_urgent_record_t* event = find_event(COMM_TX_CH_INPUT);
    assert(event != NULL);
    assert(event->payload[APP_SAFETY_EVENT_CAUSE_INDEX] == SAFETY_CAUSE_FATAL_ERROR);

    Safety_GetStats(&stats);
    assert(stats.estopEvents == 1U);
}

int main(void) {
    test_estop_latches_before_notifying_controls();
    test_estop_cause_maps_from_source_channel();
    test_stop_queue_failure_is_counted();
    test_input_reset_releases_only_input();
    test_sorting_reset_releases_only_sorting();
    test_both_scoped_resets_can_progress_independently();
    test_reset_times_out_and_keeps_latched();
    test_estop_during_release_relatches();
    test_reset_without_estop_is_ignored();
    test_fatal_trigger_enters_estop();
    return 0;
}
