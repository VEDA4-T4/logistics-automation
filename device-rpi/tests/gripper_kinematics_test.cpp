#include "logistics/device/gripper_kinematics.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "logistics/device/gripper_pose_config.hpp"

namespace {

using logistics::device::GripperGeometry;
using logistics::device::IkStatus;
using logistics::device::JointAngles;
using logistics::device::MinimumArmDurationMs;
using logistics::device::MinimumClawDurationMs;
using logistics::device::ParseGripperPoseConfig;
using logistics::device::PickPose;
using logistics::device::SolveForwardKinematics;
using logistics::device::SolveInverseKinematics;

[[nodiscard]] bool NearlyEqual(double first, double second, double tolerance_mm = 0.6) {
    return std::fabs(first - second) <= tolerance_mm;
}

/*
 * A geometry whose zero references make the arithmetic checkable by hand:
 * equal links, the shoulder on the base axis at the origin, and every joint
 * referenced to 90.0 degrees travelling positive.
 */
[[nodiscard]] GripperGeometry SymmetricGeometry() {
    GripperGeometry geometry{};
    geometry.shoulder_to_elbow_mm = 100.0;
    geometry.elbow_to_tcp_mm = 100.0;
    geometry.shoulder_height_mm = 0.0;
    geometry.shoulder_offset_mm = 0.0;
    return geometry;
}

void test_default_geometry_is_valid() {
    const GripperGeometry geometry{};
    assert(geometry.IsValid());
    // The measured dimensions (2026-07-30 ruler measurement), so a silent edit to
    // the defaults shows up here rather than as a wrong angle on hardware.
    assert(NearlyEqual(geometry.shoulder_to_elbow_mm, 85.0, 0.001));
    assert(NearlyEqual(geometry.elbow_to_tcp_mm, 175.0, 0.001));
    assert(NearlyEqual(geometry.shoulder_height_mm, 20.0, 0.001));
}

void test_geometry_rejects_unusable_values() {
    GripperGeometry zero_link = SymmetricGeometry();
    zero_link.shoulder_to_elbow_mm = 0.0;
    assert(!zero_link.IsValid());

    // A zero direction would divide by zero on the way back out of servo units.
    GripperGeometry zero_direction = SymmetricGeometry();
    zero_direction.elbow_direction = 0;
    assert(!zero_direction.IsValid());

    GripperGeometry inverted_limits = SymmetricGeometry();
    inverted_limits.shoulder_min_deci_deg = 1500U;
    inverted_limits.shoulder_max_deci_deg = 300U;
    assert(!inverted_limits.IsValid());

    GripperGeometry zero_speed = SymmetricGeometry();
    zero_speed.shoulder_max_speed_deci_deg_per_sec = 0U;
    assert(!zero_speed.IsValid());
}

/*
 * With equal links, a target at exactly one link length along +X has to fold the
 * arm into an equilateral triangle: the shoulder rises 60 degrees above
 * horizontal and the elbow closes to 60 degrees interior.
 */
void test_known_triangle_solution() {
    const GripperGeometry geometry = SymmetricGeometry();
    const auto solution = SolveInverseKinematics(geometry, PickPose{ .x_mm = 100.0, .y_mm = 0.0, .z_mm = 0.0 });

    assert(solution.Succeeded());
    // base: atan2(0, 100) = 0 degrees -> the 90.0 degree zero reference.
    assert(solution.angles.base_deci_deg == 900U);
    // shoulder: 0 + 60 = 60 degrees above horizontal -> 900 + 600.
    assert(solution.angles.shoulder_deci_deg == 1500U);
    // elbow: interior 60 degrees is 120 short of straight -> 1800 - 1200.
    assert(solution.angles.elbow_deci_deg == 600U);
}

/*
 * The elbow reference matters as much as the links: a fully extended arm must sit
 * exactly on the configured elbow zero, because that is what "straight" means.
 */
void test_full_stretch_leaves_the_elbow_at_its_zero() {
    const GripperGeometry geometry = SymmetricGeometry();
    const auto solution = SolveInverseKinematics(geometry, PickPose{ .x_mm = 200.0, .y_mm = 0.0, .z_mm = 0.0 });

    assert(solution.Succeeded());
    assert(solution.angles.base_deci_deg == 900U);
    assert(solution.angles.shoulder_deci_deg == 900U);
    assert(solution.angles.elbow_deci_deg == 1800U);
}

void test_reach_boundaries_are_reported_apart() {
    const GripperGeometry geometry = SymmetricGeometry();

    const auto too_far = SolveInverseKinematics(geometry, PickPose{ .x_mm = 400.0, .y_mm = 0.0, .z_mm = 0.0 });
    assert(too_far.status == IkStatus::kUnreachableTooFar);

    /*
     * Equal links can fold to zero, so the near limit only exists once the links
     * differ. This is the real arm's case: 245 - 135 = 110 mm of dead zone around
     * the shoulder that no posture can reach.
     */
    GripperGeometry unequal = SymmetricGeometry();
    unequal.shoulder_to_elbow_mm = 135.0;
    unequal.elbow_to_tcp_mm = 245.0;
    const auto too_close = SolveInverseKinematics(unequal, PickPose{ .x_mm = 50.0, .y_mm = 0.0, .z_mm = 0.0 });
    assert(too_close.status == IkStatus::kUnreachableTooClose);
}

/*
 * A target on the reach boundary must not fail on floating-point noise, and must
 * not produce a NaN that rounds into a plausible-looking angle.
 */
void test_exact_boundary_still_solves() {
    const GripperGeometry geometry = SymmetricGeometry();
    const double reach = geometry.shoulder_to_elbow_mm + geometry.elbow_to_tcp_mm;
    const auto solution = SolveInverseKinematics(geometry, PickPose{ .x_mm = reach, .y_mm = 0.0, .z_mm = 0.0 });

    assert(solution.Succeeded());
    assert(solution.angles.elbow_deci_deg == 1800U);
}

void test_non_finite_target_is_rejected() {
    const GripperGeometry geometry = SymmetricGeometry();
    const auto nan_target =
        SolveInverseKinematics(geometry, PickPose{ .x_mm = std::nan(""), .y_mm = 0.0, .z_mm = 0.0 });
    assert(nan_target.status == IkStatus::kInvalidGeometry);
}

/*
 * The firmware's joint window is narrower than the contract's, so a geometrically
 * fine solution can still be refused -- and the refusal has to name the joint.
 */
void test_joint_limit_names_the_blocking_joint() {
    GripperGeometry geometry = SymmetricGeometry();
    geometry.shoulder_max_deci_deg = 1000U;

    const auto solution = SolveInverseKinematics(geometry, PickPose{ .x_mm = 100.0, .y_mm = 0.0, .z_mm = 0.0 });
    assert(solution.status == IkStatus::kJointLimit);
    assert(solution.blocking_joint == std::string("shoulder"));
}

void test_base_yaw_follows_the_target_quadrant() {
    // 45 degrees off +X, at the same 150 mm radius the reach tests use. Note that
    // a target straight along +Y would need 90 degrees of yaw and the firmware's
    // base window stops at 1700, i.e. 80 degrees -- see the sibling test below.
    const double diagonal_mm = 150.0 / std::sqrt(2.0);

    const auto solution = SolveInverseKinematics(
        SymmetricGeometry(), PickPose{ .x_mm = diagonal_mm, .y_mm = diagonal_mm, .z_mm = 0.0 });
    assert(solution.Succeeded());
    assert(solution.angles.base_deci_deg == 1350U);

    // Mirroring the linkage must be an INI change, not a code change.
    GripperGeometry mirrored = SymmetricGeometry();
    mirrored.base_direction = -1;
    const auto flipped =
        SolveInverseKinematics(mirrored, PickPose{ .x_mm = diagonal_mm, .y_mm = diagonal_mm, .z_mm = 0.0 });
    assert(flipped.Succeeded());
    assert(flipped.angles.base_deci_deg == 450U);
}

/*
 * The base's mechanical window is narrower than a full half-turn, so a target
 * off to the side is reachable in distance but not in yaw. Worth pinning down
 * because it is a limit the server has to design its layout around: the arm
 * cannot serve a conveyor at 90 degrees to its zero direction.
 */
void test_yaw_beyond_the_base_window_is_a_joint_limit() {
    const auto solution =
        SolveInverseKinematics(SymmetricGeometry(), PickPose{ .x_mm = 0.0, .y_mm = 150.0, .z_mm = 0.0 });
    assert(solution.status == IkStatus::kJointLimit);
    assert(solution.blocking_joint == std::string("base"));
}

/*
 * Round-tripping is the check that catches a sign or reference error that the
 * hand-computed cases above happen to be symmetric about.
 */
void test_solution_round_trips_through_forward_kinematics() {
    /*
     * The real link lengths, but the contract's full joint range rather than the
     * firmware's narrower window. This test is about the arithmetic being
     * self-consistent; which of these points the assembled arm may actually visit
     * is the joint-limit test's job, and mixing the two would hide a sign error
     * behind a limit rejection.
     */
    GripperGeometry geometry{};
    geometry.base_min_deci_deg = 0U;
    geometry.base_max_deci_deg = UART_GRIPPER_ANGLE_DECI_DEG_MAX;
    geometry.shoulder_min_deci_deg = 0U;
    geometry.shoulder_max_deci_deg = UART_GRIPPER_ANGLE_DECI_DEG_MAX;
    geometry.elbow_min_deci_deg = 0U;
    geometry.elbow_max_deci_deg = UART_GRIPPER_ANGLE_DECI_DEG_MAX;

    // Reach is now 90 to 260 mm (85 + 175 out, 175 - 85 near). Staying inside
    // that band is not enough on its own, though: a target close to the shoulder
    // axis needs the shoulder to swing past its +-90 degree reference to reach it
    // (see test_near_and_high_targets_exceed_the_shoulder_reference), so these
    // also keep enough horizontal distance to stay clear of that.
    const PickPose targets[] = {
        PickPose{ .x_mm = 180.0, .y_mm = 0.0, .z_mm = 40.0 },
        PickPose{ .x_mm = 200.0, .y_mm = 40.0, .z_mm = 40.0 },
        PickPose{ .x_mm = 190.0, .y_mm = -60.0, .z_mm = 60.0 },
        PickPose{ .x_mm = 210.0, .y_mm = 30.0, .z_mm = 50.0 },
    };

    for (const PickPose& target : targets) {
        const auto solution = SolveInverseKinematics(geometry, target);
        if (!solution.Succeeded()) {
            std::printf("target (%.1f, %.1f, %.1f) did not solve: %s\n", target.x_mm, target.y_mm, target.z_mm,
                        std::string(logistics::device::ToString(solution.status)).c_str());
            assert(false);
            continue;
        }

        const auto recovered = SolveForwardKinematics(geometry, solution.angles);
        assert(recovered.has_value());
        // The tolerance is the rounding to whole deci-degrees, which at a 380 mm
        // reach is a few tenths of a millimetre.
        assert(NearlyEqual(recovered->x_mm, target.x_mm));
        assert(NearlyEqual(recovered->y_mm, target.y_mm));
        assert(NearlyEqual(recovered->z_mm, target.z_mm));
    }
}

/*
 * A consequence of this frame that is easy to miss: the forearm (245 mm to the
 * grasp centre) is much longer than the upper arm (135 mm), so reaching a point
 * that is close in but high up needs the upper arm to lean back past vertical.
 * The shoulder's zero reference sits at 90.0 degrees, so "past vertical" is past
 * 180.0 degrees and cannot be expressed at all.
 *
 * This is geometry, not a bug, and it is pinned here because the failure is
 * otherwise indistinguishable from a calibration mistake.
 */
void test_near_and_high_targets_exceed_the_shoulder_reference() {
    const GripperGeometry geometry{};
    const auto solution = SolveInverseKinematics(geometry, PickPose{ .x_mm = 180.0, .y_mm = -60.0, .z_mm = 120.0 });

    assert(solution.status == IkStatus::kJointLimit);
    assert(solution.blocking_joint == std::string("shoulder"));
}

/*
 * These two mirror gripper_control_arm_minimum_duration() and
 * gripper_control_claw_minimum_duration() in the firmware. If the firmware's
 * limits are retuned without updating the node, this is what should fail.
 */
void test_minimum_duration_matches_the_firmware_formula() {
    const GripperGeometry geometry{};

    const JointAngles home = geometry.HomeAngles();
    assert(MinimumArmDurationMs(geometry, home, home) == 0U);

    // Shoulder travels 600 deci-degrees at 120 per second -> 5000 ms, and it is
    // slower than the base's 600 at 300 per second (2000 ms).
    const JointAngles shoulder_move{ 900U, 1500U, 900U };
    assert(MinimumArmDurationMs(geometry, home, shoulder_move) == 5000U);

    // The slowest joint sets the pace even when another moves further.
    const JointAngles combined{ 1700U, 1500U, 900U };
    assert(MinimumArmDurationMs(geometry, home, combined) == 5000U);

    // Direction must not matter.
    assert(MinimumArmDurationMs(geometry, shoulder_move, home) == 5000U);

    // The firmware rounds up, so a travel that does not divide evenly must not be
    // predicted a millisecond short.
    const JointAngles uneven{ 900U, 901U, 900U };
    assert(MinimumArmDurationMs(geometry, home, uneven) == 9U);

    // 70 percent of claw travel at 40 percent per second, rounded up.
    assert(MinimumClawDurationMs(geometry, 100U, 30U) == 1750U);
    assert(MinimumClawDurationMs(geometry, 30U, 30U) == 0U);
}

/*
 * The worst case has to stay inside the contract's ceiling, otherwise the node
 * would send a duration the firmware refuses outright.
 */
void test_worst_case_arm_travel_fits_the_contract_ceiling() {
    const GripperGeometry geometry{};
    const JointAngles low{ geometry.base_min_deci_deg, geometry.shoulder_min_deci_deg, geometry.elbow_min_deci_deg };
    const JointAngles high{ geometry.base_max_deci_deg, geometry.shoulder_max_deci_deg, geometry.elbow_max_deci_deg };

    const std::uint32_t worst = MinimumArmDurationMs(geometry, low, high);
    assert(worst == UART_GRIPPER_DURATION_MS_MAX);
}

void test_geometry_comes_from_the_ini_section() {
    const std::string contents =
        "[gripper]\n"
        "link_shoulder_to_elbow_mm=135\n"
        "link_elbow_to_tcp_mm=248.5\n"
        "shoulder_height_mm=45\n"
        "shoulder_offset_mm=12\n"
        "elbow_direction=-1\n"
        "approach_height_mm=75\n"
        "min_target_z_mm=5\n";

    const auto config = ParseGripperPoseConfig(contents, "<test>");
    assert(NearlyEqual(config.geometry.elbow_to_tcp_mm, 248.5, 0.001));
    assert(NearlyEqual(config.geometry.shoulder_offset_mm, 12.0, 0.001));
    assert(config.geometry.elbow_direction == -1);
    assert(NearlyEqual(config.approach_height_mm, 75.0, 0.001));

    const auto approach = config.ApproachAbove(PickPose{ .x_mm = 200.0, .y_mm = 0.0, .z_mm = 30.0 });
    assert(NearlyEqual(approach.z_mm, 105.0, 0.001));
    assert(NearlyEqual(approach.x_mm, 200.0, 0.001));
}

void test_config_rejects_an_unusable_approach_height() {
    // A zero clearance would put the approach pose on the target itself, so the
    // claw would sweep in horizontally instead of descending.
    bool threw = false;
    try {
        static_cast<void>(ParseGripperPoseConfig("[gripper]\napproach_height_mm=0\n", "<test>"));
    } catch (const logistics::device::GripperConfigError&) {
        threw = true;
    }
    assert(threw);
}

}  // namespace

int main() {
    test_default_geometry_is_valid();
    test_geometry_rejects_unusable_values();
    test_known_triangle_solution();
    test_full_stretch_leaves_the_elbow_at_its_zero();
    test_reach_boundaries_are_reported_apart();
    test_exact_boundary_still_solves();
    test_non_finite_target_is_rejected();
    test_joint_limit_names_the_blocking_joint();
    test_base_yaw_follows_the_target_quadrant();
    test_yaw_beyond_the_base_window_is_a_joint_limit();
    test_solution_round_trips_through_forward_kinematics();
    test_near_and_high_targets_exceed_the_shoulder_reference();
    test_minimum_duration_matches_the_firmware_formula();
    test_worst_case_arm_travel_fits_the_contract_ceiling();
    test_geometry_comes_from_the_ini_section();
    test_config_rejects_an_unusable_approach_height();

    std::printf("gripper kinematics tests passed\n");
    return 0;
}
