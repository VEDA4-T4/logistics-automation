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

    assert(correction == 150);
    assert(pid.initialized != 0U);
    assert(pid.previous_error == 400);
    assert(pid.last_update_ms == 100U);
    assert(pid.integral == 0.0F);
}

void TestDerivativeAndPeriodScaling() {
    line_follow_pid_t pid{};

    LineFollowPid_Init(&pid);
    assert(LineFollowPid_Update(&pid, 400, 100U) == 150);
    assert(LineFollowPid_Update(&pid, 0, 110U) == -12);

    LineFollowPid_Reset(&pid);
    assert(LineFollowPid_Update(&pid, 0, 200U) == 0);
    assert(LineFollowPid_Update(&pid, 400, 220U) == 156);
}

void TestCorrectionClamp() {
    line_follow_pid_t pid{};
    constexpr auto kCorrectionLimit = static_cast<int16_t>(LINE_FOLLOW_PID_CORRECTION_LIMIT);

    LineFollowPid_Init(&pid);
    assert(LineFollowPid_Update(&pid, 1000, 100U) == kCorrectionLimit);
    assert(LineFollowPid_Update(&pid, -1000, 110U) == -kCorrectionLimit);
    assert(pid.integral <= LINE_FOLLOW_PID_INTEGRAL_LIMIT);
    assert(pid.integral >= -LINE_FOLLOW_PID_INTEGRAL_LIMIT);
}

void TestResetClearsDerivativeHistory() {
    line_follow_pid_t pid{};
    constexpr auto kCorrectionLimit = static_cast<int16_t>(LINE_FOLLOW_PID_CORRECTION_LIMIT);

    LineFollowPid_Init(&pid);
    assert(LineFollowPid_Update(&pid, 1000, 100U) == kCorrectionLimit);

    LineFollowPid_Reset(&pid);
    assert(pid.initialized == 0U);
    assert(pid.previous_error == 0);
    assert(pid.integral == 0.0F);
    assert(pid.last_update_ms == 0U);
    assert(LineFollowPid_Update(&pid, -400, 200U) == -150);
}

}  // namespace

int main() {
    TestInitializationAndFirstSample();
    TestDerivativeAndPeriodScaling();
    TestCorrectionClamp();
    TestResetClearsDerivativeHistory();
    return 0;
}
