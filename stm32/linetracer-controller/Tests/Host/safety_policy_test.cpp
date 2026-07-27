#include <cassert>

extern "C" {
#include "safety_policy.h"
}

namespace {

app_control_snapshot_t MakeControlSnapshot(uart_linetracer_state_t state) {
    app_control_snapshot_t snapshot{};
    snapshot.job_id = 1U;
    snapshot.route_id = UART_LINETRACER_ROUTE_A;
    snapshot.state = state;
    return snapshot;
}

void TestLineLossOnlyAppliesWhileFollowingOrCorrecting() {
    auto snapshot = MakeControlSnapshot(UART_LINETRACER_STATE_IDLE);

    assert(SafetyPolicy_LineLossApplies(&snapshot) == 0U);
    snapshot.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    assert(SafetyPolicy_LineLossApplies(&snapshot) == 1U);
    snapshot.state = UART_LINETRACER_STATE_CORRECTING;
    assert(SafetyPolicy_LineLossApplies(&snapshot) == 1U);
    snapshot.state = UART_LINETRACER_STATE_STOPPED;
    assert(SafetyPolicy_LineLossApplies(&snapshot) == 0U);

    snapshot.state = UART_LINETRACER_STATE_FOLLOWING_LINE;
    snapshot.job_id = UART_LINETRACER_JOB_ID_NONE;
    assert(SafetyPolicy_LineLossApplies(&snapshot) == 0U);
    snapshot.job_id = 1U;
    snapshot.route_id = UART_LINETRACER_ROUTE_NONE;
    assert(SafetyPolicy_LineLossApplies(&snapshot) == 0U);
}

void TestOnlyQueuedCommRxEstopIsMomentary() {
    app_safety_event_t event{};
    event.type = APP_SAFETY_EVENT_EMERGENCY_STOP;
    event.source_task = APP_TASK_COMM_RX;
    event.active = 1U;

    assert(SafetyPolicy_IsMomentaryRemoteEstop(&event) == 1U);
    event.source_task = APP_TASK_SAFETY;
    assert(SafetyPolicy_IsMomentaryRemoteEstop(&event) == 0U);
    event.source_task = APP_TASK_COMM_RX;
    event.active = 0U;
    assert(SafetyPolicy_IsMomentaryRemoteEstop(&event) == 0U);
}

}  // namespace

int main() {
    TestLineLossOnlyAppliesWhileFollowingOrCorrecting();
    TestOnlyQueuedCommRxEstopIsMomentary();
    return 0;
}
