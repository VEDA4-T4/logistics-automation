#include "motor_control_logic.h"

#include <cassert>

#include "motor_control_config.h"

namespace {

void AssertForward(const motor_output_t& output, uint16_t left_pwm, uint16_t right_pwm) {
    assert(output.standby != 0U);
    assert(output.left_direction == MOTOR_DIRECTION_FORWARD);
    assert(output.right_direction == MOTOR_DIRECTION_FORWARD);
    assert(output.left_pwm == left_pwm);
    assert(output.right_pwm == right_pwm);
}

void TestClamp() {
    assert(MotorControlLogic_ClampPwm(-1) == 0U);
    assert(MotorControlLogic_ClampPwm(0) == 0U);
    assert(MotorControlLogic_ClampPwm(500) == 500U);
    assert(MotorControlLogic_ClampPwm(1200) == MOTOR_CONTROL_PWM_MAX);
}

void TestSafeStop() {
    motor_output_t output{};

    output.left_pwm = 700U;
    output.right_pwm = 700U;
    output.left_direction = MOTOR_DIRECTION_FORWARD;
    output.right_direction = MOTOR_DIRECTION_FORWARD;
    output.standby = 1U;
    MotorControlLogic_MakeSafeStop(&output);

    assert(output.left_pwm == 0U);
    assert(output.right_pwm == 0U);
    assert(output.left_direction == MOTOR_DIRECTION_COAST);
    assert(output.right_direction == MOTOR_DIRECTION_COAST);
    assert(output.standby == 0U);
}

void TestLineFollow() {
    motor_output_t output{};

    assert(MotorControlLogic_ComputeLineFollow(LINETRACER_LINE_CENTERED, &output) != 0U);
    AssertForward(output, MotorControlLogic_ClampPwm(MOTOR_CONTROL_LEFT_BASE_PWM + MOTOR_CONTROL_LEFT_TRIM),
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_RIGHT_BASE_PWM + MOTOR_CONTROL_RIGHT_TRIM));

    assert(MotorControlLogic_ComputeLineFollow(LINETRACER_LINE_LEFT_ONLY, &output) != 0U);
    AssertForward(output,
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_LEFT_BASE_PWM - MOTOR_CONTROL_LINE_RECOVERY_CORRECTION_PWM +
                                             MOTOR_CONTROL_LEFT_TRIM),
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_RIGHT_BASE_PWM +
                                             MOTOR_CONTROL_RIGHT_TRACKING_FAST_BOOST_PWM + MOTOR_CONTROL_RIGHT_TRIM));

    assert(MotorControlLogic_ComputeLineFollow(LINETRACER_LINE_RIGHT_ONLY, &output) != 0U);
    AssertForward(output,
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_LEFT_BASE_PWM + MOTOR_CONTROL_LEFT_TRACKING_FAST_BOOST_PWM +
                                             MOTOR_CONTROL_LEFT_TRIM),
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_RIGHT_BASE_PWM - MOTOR_CONTROL_LINE_RECOVERY_CORRECTION_PWM +
                                             MOTOR_CONTROL_RIGHT_TRIM));
}

void TestDifferentialForward() {
    motor_output_t output{};

    assert(MotorControlLogic_ComputeDifferentialForward(300U, 305U, 100, &output) != 0U);
    AssertForward(
        output, MotorControlLogic_ClampPwm(200 + MOTOR_CONTROL_LEFT_TRIM),
        MotorControlLogic_ClampPwm(305 + MOTOR_CONTROL_RIGHT_TRACKING_FAST_BOOST_PWM + MOTOR_CONTROL_RIGHT_TRIM));

    assert(MotorControlLogic_ComputeDifferentialForward(300U, 305U, -100, &output) != 0U);
    AssertForward(
        output, MotorControlLogic_ClampPwm(300 + MOTOR_CONTROL_LEFT_TRACKING_FAST_BOOST_PWM + MOTOR_CONTROL_LEFT_TRIM),
        MotorControlLogic_ClampPwm(205 + MOTOR_CONTROL_RIGHT_TRIM));
}

void TestDifferentialForwardKeepsBothWheelsTurning() {
    motor_output_t output{};

    assert(MotorControlLogic_ComputeDifferentialForward(240U, 265U, 1000, &output) != 0U);
    AssertForward(
        output, MotorControlLogic_ClampPwm(MOTOR_CONTROL_TRACKING_MIN_PWM + MOTOR_CONTROL_LEFT_TRIM),
        MotorControlLogic_ClampPwm(265 + MOTOR_CONTROL_RIGHT_TRACKING_FAST_BOOST_PWM + MOTOR_CONTROL_RIGHT_TRIM));

    assert(MotorControlLogic_ComputeDifferentialForward(240U, 265U, -1000, &output) != 0U);
    AssertForward(
        output, MotorControlLogic_ClampPwm(240 + MOTOR_CONTROL_LEFT_TRACKING_FAST_BOOST_PWM + MOTOR_CONTROL_LEFT_TRIM),
        MotorControlLogic_ClampPwm(MOTOR_CONTROL_TRACKING_MIN_PWM + MOTOR_CONTROL_RIGHT_TRIM));
}

void TestWhiteGapKeepsPreviousOutput() {
    motor_output_t output{};

    output.left_pwm = 321U;
    output.right_pwm = 654U;
    output.left_direction = MOTOR_DIRECTION_FORWARD;
    output.right_direction = MOTOR_DIRECTION_FORWARD;
    output.standby = 1U;

    assert(MotorControlLogic_ComputeLineFollow(LINETRACER_LINE_WHITE_GAP, &output) == 0U);
    assert(output.left_pwm == 321U);
    assert(output.right_pwm == 654U);
    assert(output.standby != 0U);
    assert(MotorControlLogic_ComputeLineFollow(LINETRACER_LINE_UNKNOWN, &output) == 0U);
    assert(output.left_pwm == 321U);
    assert(output.right_pwm == 654U);
}

void TestRouteActions() {
    motor_output_t output{};

    assert(MotorControlLogic_ComputeRouteAction(ROUTE_ACTION_GO_STRAIGHT, &output) != 0U);
    AssertForward(output, MotorControlLogic_ClampPwm(MOTOR_CONTROL_LEFT_BASE_PWM + MOTOR_CONTROL_LEFT_TRIM),
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_RIGHT_BASE_PWM + MOTOR_CONTROL_RIGHT_TRIM));

    assert(MotorControlLogic_ComputeRouteAction(ROUTE_ACTION_TURN_LEFT, &output) != 0U);
    assert(output.left_direction == MOTOR_DIRECTION_REVERSE);
    assert(output.right_direction == MOTOR_DIRECTION_FORWARD);
    assert(output.left_pwm == MotorControlLogic_ClampPwm(MOTOR_CONTROL_LEFT_PIVOT_PWM + MOTOR_CONTROL_LEFT_TRIM));
    assert(output.right_pwm ==
           MotorControlLogic_ClampPwm(MOTOR_CONTROL_RIGHT_PIVOT_PWM + MOTOR_CONTROL_LEFT_TURN_RIGHT_BOOST_PWM +
                                      MOTOR_CONTROL_RIGHT_TRIM));
    assert(output.standby != 0U);

    assert(MotorControlLogic_ComputeRouteAction(ROUTE_ACTION_TURN_RIGHT, &output) != 0U);
    assert(output.left_direction == MOTOR_DIRECTION_FORWARD);
    assert(output.right_direction == MOTOR_DIRECTION_REVERSE);
    assert(output.standby != 0U);

    assert(MotorControlLogic_ComputeRouteAction(ROUTE_ACTION_TURN_AROUND, &output) != 0U);
    assert(output.left_direction == MOTOR_DIRECTION_FORWARD);
    assert(output.right_direction == MOTOR_DIRECTION_REVERSE);
    assert(output.left_pwm == MotorControlLogic_ClampPwm(MOTOR_CONTROL_LEFT_UTURN_PWM + MOTOR_CONTROL_LEFT_TRIM));
    assert(output.right_pwm == MotorControlLogic_ClampPwm(MOTOR_CONTROL_RIGHT_UTURN_PWM + MOTOR_CONTROL_RIGHT_TRIM));
    assert(output.standby != 0U);
}

void TestStopActions() {
    const route_action_t actions[] = { ROUTE_ACTION_STOP_AT_PICKUP, ROUTE_ACTION_STOP_AT_DEST,
                                       ROUTE_ACTION_JOB_COMPLETE, ROUTE_ACTION_LOAD_LOST, ROUTE_ACTION_ERROR };

    for (const auto action : actions) {
        motor_output_t output{};
        output.standby = 1U;
        output.left_pwm = 500U;
        output.right_pwm = 500U;
        assert(MotorControlLogic_ComputeRouteAction(action, &output) != 0U);
        assert(output.standby == 0U);
        assert(output.left_pwm == 0U);
        assert(output.right_pwm == 0U);
    }
}

void TestControlStateOutputPriority() {
    motor_output_t output{};

    assert(MotorControlLogic_ComputeControlOutput(LINETRACER_CONTROL_MOVING_TO_DEST, ROUTE_ACTION_GO_STRAIGHT,
                                                  LINETRACER_LINE_CENTERED, 1U, 1U, &output) != 0U);
    assert(output.standby == 0U);

    assert(MotorControlLogic_ComputeControlOutput(LINETRACER_CONTROL_STOPPED, ROUTE_ACTION_GO_STRAIGHT,
                                                  LINETRACER_LINE_CENTERED, 1U, 0U, &output) != 0U);
    assert(output.standby == 0U);

    assert(MotorControlLogic_ComputeControlOutput(LINETRACER_CONTROL_TURNING_FROM_DEST, ROUTE_ACTION_TURN_AROUND,
                                                  LINETRACER_LINE_UNKNOWN, 1U, 0U, &output) != 0U);
    assert(output.standby != 0U);
    assert(output.left_direction == MOTOR_DIRECTION_FORWARD);
    assert(output.right_direction == MOTOR_DIRECTION_REVERSE);

    assert(MotorControlLogic_ComputeControlOutput(LINETRACER_CONTROL_MOVING_ON_COMMON_LINE, ROUTE_ACTION_TURN_LEFT,
                                                  LINETRACER_LINE_CENTERED, 1U, 0U, &output) != 0U);
    assert(output.left_direction == MOTOR_DIRECTION_REVERSE);
    assert(output.right_direction == MOTOR_DIRECTION_FORWARD);

    assert(MotorControlLogic_ComputeControlOutput(LINETRACER_CONTROL_MOVING_TO_DEST, ROUTE_ACTION_GO_STRAIGHT,
                                                  LINETRACER_LINE_RIGHT_ONLY, 1U, 0U, &output) != 0U);
    AssertForward(output,
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_LEFT_BASE_PWM + MOTOR_CONTROL_LEFT_TRACKING_FAST_BOOST_PWM +
                                             MOTOR_CONTROL_LEFT_TRIM),
                  MotorControlLogic_ClampPwm(MOTOR_CONTROL_RIGHT_BASE_PWM - MOTOR_CONTROL_LINE_RECOVERY_CORRECTION_PWM +
                                             MOTOR_CONTROL_RIGHT_TRIM));
}

void TestControlWhiteGapHoldsPreviousOutput() {
    motor_output_t output{};

    output.left_pwm = 444U;
    output.right_pwm = 555U;
    output.left_direction = MOTOR_DIRECTION_FORWARD;
    output.right_direction = MOTOR_DIRECTION_FORWARD;
    output.standby = 1U;
    assert(MotorControlLogic_ComputeControlOutput(LINETRACER_CONTROL_MOVING_TO_PICKUP, ROUTE_ACTION_GO_STRAIGHT,
                                                  LINETRACER_LINE_WHITE_GAP, 1U, 0U, &output) == 0U);
    assert(output.left_pwm == 444U);
    assert(output.right_pwm == 555U);
    assert(output.standby != 0U);
}

}  // namespace

void RunMotorControlLogicTests() {
    TestClamp();
    TestSafeStop();
    TestLineFollow();
    TestDifferentialForward();
    TestDifferentialForwardKeepsBothWheelsTurning();
    TestWhiteGapKeepsPreviousOutput();
    TestRouteActions();
    TestStopActions();
    TestControlStateOutputPriority();
    TestControlWhiteGapHoldsPreviousOutput();
}
