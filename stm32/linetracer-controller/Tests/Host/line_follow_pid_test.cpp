#include "line_follow_pid.h"

#include <cassert>

namespace {

void TestInitializationAndFirstSample() {
    line_follow_pid_t pid{};

    LineFollowPid_Init(&pid);
    assert(pid.initialized == 0U);
    assert(pid.previous_error == 0);
    assert(pid.integral == 0.0F);

    const auto correction = LineFollowPid_Update(&pid, 400, 100U);

    assert(correction == 100);
    assert(pid.initialized != 0U);
    assert(pid.previous_error == 400);
    assert(pid.last_update_ms == 100U);
    assert(pid.integral == 0.0F);
}

void TestDerivativeAndPeriodScaling() {
    line_follow_pid_t pid{};

    LineFollowPid_Init(&pid);
    assert(LineFollowPid_Update(&pid, 400, 100U) == 100);
    assert(LineFollowPid_Update(&pid, 0, 110U) == -20);

    LineFollowPid_Reset(&pid);
    assert(LineFollowPid_Update(&pid, 0, 200U) == 0);
    assert(LineFollowPid_Update(&pid, 400, 220U) == 110);
}

void TestCorrectionClamp() {
    line_follow_pid_t pid{};

    LineFollowPid_Init(&pid);
    assert(LineFollowPid_Update(&pid, 1000, 100U) == 250);
    assert(LineFollowPid_Update(&pid, -1000, 110U) == -250);
    assert(pid.integral <= LINE_FOLLOW_PID_INTEGRAL_LIMIT);
    assert(pid.integral >= -LINE_FOLLOW_PID_INTEGRAL_LIMIT);
}

void TestResetClearsDerivativeHistory() {
    line_follow_pid_t pid{};

    LineFollowPid_Init(&pid);
    assert(LineFollowPid_Update(&pid, 1000, 100U) == 250);

    LineFollowPid_Reset(&pid);
    assert(pid.initialized == 0U);
    assert(pid.previous_error == 0);
    assert(pid.integral == 0.0F);
    assert(pid.last_update_ms == 0U);
    assert(LineFollowPid_Update(&pid, -400, 200U) == -100);
}

}  // namespace

int main() {
    TestInitializationAndFirstSample();
    TestDerivativeAndPeriodScaling();
    TestCorrectionClamp();
    TestResetClearsDerivativeHistory();
    return 0;
}
