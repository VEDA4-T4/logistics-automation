#include <cassert>
#include <cstdint>

extern "C" {
#include "health_config.h"
#include "health_logic.h"
}

static app_health_event_t MakeEvent(app_task_id_t source, app_health_event_type_t type, uint32_t now_ms,
                                    uint32_t detail) {
    app_health_event_t event{};

    event.source_task = source;
    event.type = type;
    event.occurred_at_ms = now_ms;
    event.detail = detail;
    return event;
}

static void PublishRequiredAlive(health_logic_context_t* context, uint32_t now_ms, app_task_id_t excluded) {
    for (uint32_t task = 0U; task < static_cast<uint32_t>(APP_TASK_COUNT); ++task) {
        const uint32_t task_bit = HEALTH_TASK_MASK(task);

        if ((HEALTH_REQUIRED_TASK_MASK & task_bit) == 0U || task == static_cast<uint32_t>(excluded)) {
            continue;
        }

        const app_health_event_t event =
            MakeEvent(static_cast<app_task_id_t>(task), APP_HEALTH_EVENT_TASK_ALIVE, now_ms, task);
        health_fault_record_t fault{};
        assert(HealthLogic_HandleEvent(context, &event, &fault) == 0U);
    }
}

static void SetRequiredStack(health_logic_context_t* context, uint32_t high_water_words) {
    for (uint32_t task = 0U; task < static_cast<uint32_t>(APP_TASK_COUNT); ++task) {
        if ((HEALTH_REQUIRED_TASK_MASK & HEALTH_TASK_MASK(task)) != 0U) {
            HealthLogic_UpdateStack(context, static_cast<app_task_id_t>(task), high_water_words);
        }
    }
}

static void TestStartupGraceAllowsWatchdog(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 100U);
    assert(HealthLogic_Evaluate(&context, 100U + HEALTH_STARTUP_GRACE_MS - 1U, HEALTH_STARTUP_GRACE_MS,
                                HEALTH_ALIVE_TIMEOUT_MS, HEALTH_STACK_MIN_WORDS, &fault) == 0U);
    assert(HealthLogic_WatchdogAllowed(&context, 100U + HEALTH_STARTUP_GRACE_MS - 1U, HEALTH_STARTUP_GRACE_MS,
                                       HEALTH_ALIVE_TIMEOUT_MS, HEALTH_STACK_MIN_WORDS) != 0U);
}

static void TestAllRequiredTasksAllowWatchdog(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    PublishRequiredAlive(&context, 1000U, APP_TASK_COUNT);
    SetRequiredStack(&context, HEALTH_STACK_MIN_WORDS + 64U);

    assert(HealthLogic_Evaluate(&context, 2000U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                HEALTH_STACK_MIN_WORDS, &fault) == 0U);
    assert(HealthLogic_WatchdogAllowed(&context, 2000U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                       HEALTH_STACK_MIN_WORDS) != 0U);
}

static void TestTaskStallIsBlocking(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    PublishRequiredAlive(&context, 1000U, APP_TASK_CONTROL);
    SetRequiredStack(&context, HEALTH_STACK_MIN_WORDS + 64U);

    assert(HealthLogic_Evaluate(&context, 2000U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                HEALTH_STACK_MIN_WORDS, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_TASK_STALLED);
    assert(fault.source_task == APP_TASK_CONTROL);
    assert(fault.error_code == UART_ERROR_INTERNAL);
    assert(fault.watchdog_blocking != 0U);
    assert(HealthLogic_WatchdogAllowed(&context, 2000U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                       HEALTH_STACK_MIN_WORDS) == 0U);

    assert(HealthLogic_Evaluate(&context, 2100U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                HEALTH_STACK_MIN_WORDS, &fault) == 0U);
}

static void TestStackLowIsBlocking(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    PublishRequiredAlive(&context, 1000U, APP_TASK_COUNT);
    SetRequiredStack(&context, HEALTH_STACK_MIN_WORDS + 64U);
    HealthLogic_UpdateStack(&context, APP_TASK_SENSOR, HEALTH_STACK_MIN_WORDS - 1U);

    assert(HealthLogic_Evaluate(&context, 2000U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                HEALTH_STACK_MIN_WORDS, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_STACK_LOW);
    assert(fault.source_task == APP_TASK_SENSOR);
    assert(fault.detail == HEALTH_STACK_MIN_WORDS - 1U);
    assert(fault.watchdog_blocking != 0U);
}

static void TestEventKindsRemainDistinct(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);

    app_health_event_t event = MakeEvent(APP_TASK_CONTROL, APP_HEALTH_EVENT_QUEUE_FULL, 10U, 3U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_QUEUE_OVERFLOW);
    assert(fault.error_code == UART_ERROR_BUSY);
    assert(fault.watchdog_blocking == 0U);

    event = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_TIMEOUT, 20U, 5000U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_UART_RX_TIMEOUT);
    assert(fault.error_code == UART_ERROR_TIMEOUT);
    assert(fault.watchdog_blocking == 0U);

    event = MakeEvent(APP_TASK_COMM_TX, APP_HEALTH_EVENT_UART_TX_TIMEOUT, 30U, 19U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_UART_TX_TIMEOUT);
    assert(fault.error_code == UART_ERROR_TIMEOUT);
    assert(fault.watchdog_blocking == 0U);

    event = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_ERROR, 40U, 7U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_UART_RX_ERROR);
    assert(fault.error_code == UART_ERROR_INTERNAL);
    assert(fault.watchdog_blocking == 0U);

    event = MakeEvent(APP_TASK_COMM_TX, APP_HEALTH_EVENT_UART_TX_ERROR, 50U, 8U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_UART_TX_ERROR);
    assert(fault.error_code == UART_ERROR_INTERNAL);
    assert(fault.watchdog_blocking == 0U);
    assert(HealthLogic_HasActiveFaults(&context) != 0U);
}

static void TestQueueFaultClearsAfterQuietPeriodAndCanRepeat(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};
    app_health_event_t event = MakeEvent(APP_TASK_CONTROL, APP_HEALTH_EVENT_QUEUE_FULL, 100U, 3U);

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);

    event.occurred_at_ms = 1000U;
    assert(HealthLogic_HandleEvent(&context, &event, &fault) == 0U);
    assert(HealthLogic_ClearExpiredTransientFaults(&context, 1000U + HEALTH_TRANSIENT_FAULT_CLEAR_MS - 1U,
                                                   HEALTH_TRANSIENT_FAULT_CLEAR_MS) == 0U);
    assert(HealthLogic_HasActiveFaults(&context) != 0U);

    assert(HealthLogic_ClearExpiredTransientFaults(&context, 1000U + HEALTH_TRANSIENT_FAULT_CLEAR_MS,
                                                   HEALTH_TRANSIENT_FAULT_CLEAR_MS) == 1U);
    assert(HealthLogic_HasActiveFaults(&context) == 0U);

    event.occurred_at_ms = 4000U;
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
}

static void TestUartRxTimeoutRequiresExplicitRecovery(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};
    app_health_event_t event = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_TIMEOUT, 100U, 5000U);

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(HealthLogic_ClearExpiredTransientFaults(&context, 100U + HEALTH_TRANSIENT_FAULT_CLEAR_MS,
                                                   HEALTH_TRANSIENT_FAULT_CLEAR_MS) == 0U);
    assert(HealthLogic_HasActiveFaults(&context) != 0U);

    event = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_RECOVERED, 3000U, 1U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) == 0U);
    assert(HealthLogic_HasActiveFaults(&context) == 0U);
}

static void TestOneRecoveryDoesNotClearAnotherFault(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};
    app_health_event_t rx_timeout = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_TIMEOUT, 100U, 5000U);
    app_health_event_t tx_timeout = MakeEvent(APP_TASK_COMM_TX, APP_HEALTH_EVENT_UART_TX_TIMEOUT, 200U, 19U);
    app_health_event_t rx_recovered = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_RECOVERED, 300U, 1U);

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    assert(HealthLogic_HandleEvent(&context, &rx_timeout, &fault) != 0U);
    assert(HealthLogic_HandleEvent(&context, &tx_timeout, &fault) != 0U);
    assert(HealthLogic_HandleEvent(&context, &rx_recovered, &fault) == 0U);
    assert(HealthLogic_HasActiveFaults(&context) != 0U);

    assert(HealthLogic_ClearExpiredTransientFaults(&context, 200U + HEALTH_TRANSIENT_FAULT_CLEAR_MS,
                                                   HEALTH_TRANSIENT_FAULT_CLEAR_MS) == 1U);
    assert(HealthLogic_HasActiveFaults(&context) == 0U);
}

static void TestUartRecoveryClearsTimeoutAndRecoverableError(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};
    app_health_event_t event =
        MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_TIMEOUT, 100U, 5000U);

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);

    event = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_ERROR, 200U, 7U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(HealthLogic_HasActiveFaults(&context) != 0U);

    event = MakeEvent(APP_TASK_COMM_RX, APP_HEALTH_EVENT_UART_RX_RECOVERED, 300U, 1U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) == 0U);
    assert(HealthLogic_HasActiveFaults(&context) == 0U);
}

static void TestInternalErrorBlocksAndSuppressesDuplicate(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};
    const app_health_event_t event = MakeEvent(APP_TASK_COMM_TX, APP_HEALTH_EVENT_INTERNAL_ERROR, 50U, 7U);

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) != 0U);
    assert(fault.reason == HEALTH_FAULT_INTERNAL_ERROR);
    assert(fault.error_code == UART_ERROR_INTERNAL);
    assert(fault.watchdog_blocking != 0U);
    assert(HealthLogic_HandleEvent(&context, &event, &fault) == 0U);
    assert(HealthLogic_WatchdogAllowed(&context, 100U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                       HEALTH_STACK_MIN_WORDS) == 0U);
    assert(HealthLogic_ClearExpiredTransientFaults(&context, 5000U, HEALTH_TRANSIENT_FAULT_CLEAR_MS) == 0U);
    assert(HealthLogic_HasActiveFaults(&context) != 0U);
}

static void TestUnloadIsDeferred(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};

    assert((HEALTH_REQUIRED_TASK_MASK & HEALTH_TASK_MASK(APP_TASK_UNLOAD)) == 0U);
    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, 0U);
    PublishRequiredAlive(&context, 1000U, APP_TASK_COUNT);
    SetRequiredStack(&context, HEALTH_STACK_MIN_WORDS + 64U);

    assert(HealthLogic_Evaluate(&context, 2000U, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                HEALTH_STACK_MIN_WORDS, &fault) == 0U);
}

static void TestTickWraparound(void) {
    health_logic_context_t context{};
    health_fault_record_t fault{};
    const uint32_t started_at = UINT32_MAX - 1000U;
    const uint32_t alive_at = started_at + 700U;
    const uint32_t evaluated_at = started_at + 2100U;

    HealthLogic_Init(&context, HEALTH_REQUIRED_TASK_MASK, started_at);
    PublishRequiredAlive(&context, alive_at, APP_TASK_COUNT);
    SetRequiredStack(&context, HEALTH_STACK_MIN_WORDS + 64U);

    assert(HealthLogic_Evaluate(&context, evaluated_at, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                HEALTH_STACK_MIN_WORDS, &fault) == 0U);
    assert(HealthLogic_WatchdogAllowed(&context, evaluated_at, HEALTH_STARTUP_GRACE_MS, HEALTH_ALIVE_TIMEOUT_MS,
                                       HEALTH_STACK_MIN_WORDS) != 0U);
}

static void TestBusyIsAValidLineTracerFault(void) {
    assert(uart_linetracer_fault_error_is_valid(UART_ERROR_BUSY) != 0U);
}

int main() {
    TestStartupGraceAllowsWatchdog();
    TestAllRequiredTasksAllowWatchdog();
    TestTaskStallIsBlocking();
    TestStackLowIsBlocking();
    TestEventKindsRemainDistinct();
    TestQueueFaultClearsAfterQuietPeriodAndCanRepeat();
    TestUartRxTimeoutRequiresExplicitRecovery();
    TestOneRecoveryDoesNotClearAnotherFault();
    TestUartRecoveryClearsTimeoutAndRecoverableError();
    TestInternalErrorBlocksAndSuppressesDuplicate();
    TestUnloadIsDeferred();
    TestTickWraparound();
    TestBusyIsAValidLineTracerFault();
    return 0;
}
