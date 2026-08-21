#include "logistics/device/gripper_kinematics.hpp"

#include <algorithm>
#include <cmath>

namespace logistics::device {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Link lengths below this are a configuration mistake rather than a small arm,
// and they would make the law-of-cosines denominators meaningless.
constexpr double kMinimumLinkMm = 1.0;

// Tolerance on the reach checks, in millimetres. Without it a target computed to
// sit exactly at full stretch fails on floating-point noise alone.
constexpr double kReachEpsilonMm = 1e-6;

[[nodiscard]] double ToDegrees(double radians) noexcept {
    return radians * 180.0 / kPi;
}

[[nodiscard]] double ToRadians(double degrees) noexcept {
    return degrees * kPi / 180.0;
}

/*
 * Applies a joint's zero reference and travel direction.
 *
 * Kept in one place because getting a sign wrong here is invisible in the
 * arithmetic and only shows up as the arm moving the wrong way on hardware.
 */
[[nodiscard]] double ToServoDeciDeg(double zero_deci_deg, int direction, double geometric_deg) noexcept {
    return zero_deci_deg + static_cast<double>(direction) * geometric_deg * 10.0;
}

[[nodiscard]] double FromServoDeciDeg(double zero_deci_deg, int direction, double servo_deci_deg) noexcept {
    return ((servo_deci_deg - zero_deci_deg) / static_cast<double>(direction)) / 10.0;
}

/*
 * Rounds to the nearest deci-degree and reports whether the result still fits the
 * contract's unsigned range.
 *
 * A solution that lands outside 0..1800 is a geometry or zero-reference problem,
 * and letting it wrap into a uint16 would send the arm somewhere unrelated.
 */
[[nodiscard]] bool ToContractAngle(double servo_deci_deg, std::uint16_t& out) noexcept {
    const double rounded = std::round(servo_deci_deg);
    if (!std::isfinite(rounded) || rounded < 0.0 || rounded > static_cast<double>(UART_GRIPPER_ANGLE_DECI_DEG_MAX)) {
        return false;
    }
    out = static_cast<std::uint16_t>(rounded);
    return true;
}

[[nodiscard]] std::uint32_t AbsoluteDifference(std::uint32_t first, std::uint32_t second) noexcept {
    return (first >= second) ? (first - second) : (second - first);
}

/*
 * Travel time under a speed limit, rounding up.
 *
 * Mirrors gripper_control_duration_for_delta() in the firmware exactly, including
 * the ceiling division. Rounding down here instead would leave the node expecting
 * a completion up to a millisecond before the controller produces one.
 */
[[nodiscard]] std::uint32_t DurationForDelta(std::uint32_t delta, std::uint32_t units_per_second) noexcept {
    if (delta == 0U || units_per_second == 0U) {
        return 0U;
    }
    return ((delta * 1000U) + units_per_second - 1U) / units_per_second;
}

}  // namespace

bool GripperGeometry::IsValid() const noexcept {
    const bool links_usable = std::isfinite(shoulder_to_elbow_mm) && std::isfinite(elbow_to_tcp_mm) &&
                              shoulder_to_elbow_mm >= kMinimumLinkMm && elbow_to_tcp_mm >= kMinimumLinkMm;
    const bool mounting_usable = std::isfinite(shoulder_height_mm) && std::isfinite(shoulder_offset_mm);
    const bool zeros_usable = std::isfinite(base_zero_deci_deg) && std::isfinite(shoulder_zero_deci_deg) &&
                              std::isfinite(elbow_zero_deci_deg);
    // Only the two directions are meaningful; a zero would collapse the joint and
    // divide by zero on the way back out.
    const bool directions_usable = (base_direction == 1 || base_direction == -1) &&
                                   (shoulder_direction == 1 || shoulder_direction == -1) &&
                                   (elbow_direction == 1 || elbow_direction == -1);
    const bool limits_usable = base_min_deci_deg < base_max_deci_deg && shoulder_min_deci_deg < shoulder_max_deci_deg &&
                               elbow_min_deci_deg < elbow_max_deci_deg &&
                               uart_gripper_angle_is_valid(base_max_deci_deg) != 0U &&
                               uart_gripper_angle_is_valid(shoulder_max_deci_deg) != 0U &&
                               uart_gripper_angle_is_valid(elbow_max_deci_deg) != 0U;
    // A zero speed limit would make every motion take forever, and the firmware
    // would divide by it too.
    const bool speeds_usable = base_max_speed_deci_deg_per_sec > 0U && shoulder_max_speed_deci_deg_per_sec > 0U &&
                               elbow_max_speed_deci_deg_per_sec > 0U && claw_max_speed_percent_per_sec > 0U;

    return links_usable && mounting_usable && zeros_usable && directions_usable && limits_usable && speeds_usable;
}

JointAngles GripperGeometry::HomeAngles() const noexcept {
    // Mirrors GRIPPER_*_HOME_ANGLE_DECI_DEG, which the firmware also uses as its
    // power-on assumption before the first HOME completes.
    return JointAngles{ 1000U, 700U, 1200U };
}

bool GripperGeometry::AnglesWithinLimits(const JointAngles& angles) const noexcept {
    return angles.base_deci_deg >= base_min_deci_deg && angles.base_deci_deg <= base_max_deci_deg &&
           angles.shoulder_deci_deg >= shoulder_min_deci_deg && angles.shoulder_deci_deg <= shoulder_max_deci_deg &&
           angles.elbow_deci_deg >= elbow_min_deci_deg && angles.elbow_deci_deg <= elbow_max_deci_deg;
}

IkSolution SolveInverseKinematics(const GripperGeometry& geometry, const PickPose& pose) noexcept {
    IkSolution solution{};

    if (!geometry.IsValid()) {
        solution.status = IkStatus::kInvalidGeometry;
        return solution;
    }
    if (!std::isfinite(pose.x_mm) || !std::isfinite(pose.y_mm) || !std::isfinite(pose.z_mm)) {
        solution.status = IkStatus::kInvalidGeometry;
        return solution;
    }

    const double l1 = geometry.shoulder_to_elbow_mm;
    const double l2 = geometry.elbow_to_tcp_mm;

    // Base yaw. atan2 is defined at the origin as 0, which is harmless because a
    // target on the base axis fails the reach check below anyway.
    const double base_deg = ToDegrees(std::atan2(pose.y_mm, pose.x_mm));

    /*
     * Reduce to the vertical plane that contains the arm.
     *
     * The radius is taken along the yaw direction just computed, so the shoulder
     * offset is subtracted after the rotation rather than before it.
     */
    const double radius_mm = std::hypot(pose.x_mm, pose.y_mm) - geometry.shoulder_offset_mm;
    const double height_mm = pose.z_mm - geometry.shoulder_height_mm;
    const double distance_mm = std::hypot(radius_mm, height_mm);

    if (distance_mm > l1 + l2 + kReachEpsilonMm) {
        solution.status = IkStatus::kUnreachableTooFar;
        return solution;
    }
    if (distance_mm < std::fabs(l1 - l2) - kReachEpsilonMm || distance_mm < kReachEpsilonMm) {
        solution.status = IkStatus::kUnreachableTooClose;
        return solution;
    }

    /*
     * Two-link planar solution, elbow-up.
     *
     * The cosines are clamped before acos because a target sitting exactly on
     * either reach boundary can produce 1.0 + 1e-16 and turn the whole solution
     * into a NaN that would then round into a plausible-looking angle.
     */
    const double cos_elbow = std::clamp((l1 * l1 + l2 * l2 - distance_mm * distance_mm) / (2.0 * l1 * l2), -1.0, 1.0);
    const double elbow_interior_deg = ToDegrees(std::acos(cos_elbow));

    const double cos_shoulder_offset =
        std::clamp((l1 * l1 + distance_mm * distance_mm - l2 * l2) / (2.0 * l1 * distance_mm), -1.0, 1.0);
    const double shoulder_deg = ToDegrees(std::atan2(height_mm, radius_mm)) + ToDegrees(std::acos(cos_shoulder_offset));

    // The elbow servo is referenced to a fully extended arm, so what it travels
    // is the deviation from 180 degrees rather than the interior angle itself.
    const double elbow_deviation_deg = elbow_interior_deg - 180.0;

    JointAngles angles{};
    if (!ToContractAngle(ToServoDeciDeg(geometry.base_zero_deci_deg, geometry.base_direction, base_deg),
                         angles.base_deci_deg)) {
        solution.status = IkStatus::kJointLimit;
        solution.blocking_joint = "base";
        return solution;
    }
    if (!ToContractAngle(ToServoDeciDeg(geometry.shoulder_zero_deci_deg, geometry.shoulder_direction, shoulder_deg),
                         angles.shoulder_deci_deg)) {
        solution.status = IkStatus::kJointLimit;
        solution.blocking_joint = "shoulder";
        return solution;
    }
    if (!ToContractAngle(ToServoDeciDeg(geometry.elbow_zero_deci_deg, geometry.elbow_direction, elbow_deviation_deg),
                         angles.elbow_deci_deg)) {
        solution.status = IkStatus::kJointLimit;
        solution.blocking_joint = "elbow";
        return solution;
    }

    // Now the narrower firmware limits, reported per joint so the server sees
    // which one to move away from rather than a bare rejection.
    if (angles.base_deci_deg < geometry.base_min_deci_deg || angles.base_deci_deg > geometry.base_max_deci_deg) {
        solution.status = IkStatus::kJointLimit;
        solution.blocking_joint = "base";
        return solution;
    }
    if (angles.shoulder_deci_deg < geometry.shoulder_min_deci_deg ||
        angles.shoulder_deci_deg > geometry.shoulder_max_deci_deg) {
        solution.status = IkStatus::kJointLimit;
        solution.blocking_joint = "shoulder";
        return solution;
    }
    if (angles.elbow_deci_deg < geometry.elbow_min_deci_deg || angles.elbow_deci_deg > geometry.elbow_max_deci_deg) {
        solution.status = IkStatus::kJointLimit;
        solution.blocking_joint = "elbow";
        return solution;
    }

    solution.status = IkStatus::kOk;
    solution.angles = angles;
    return solution;
}

std::optional<PickPose> SolveForwardKinematics(const GripperGeometry& geometry, const JointAngles& angles) noexcept {
    if (!geometry.IsValid()) {
        return std::nullopt;
    }

    const double base_deg = FromServoDeciDeg(geometry.base_zero_deci_deg, geometry.base_direction,
                                             static_cast<double>(angles.base_deci_deg));
    const double shoulder_deg = FromServoDeciDeg(geometry.shoulder_zero_deci_deg, geometry.shoulder_direction,
                                                 static_cast<double>(angles.shoulder_deci_deg));
    const double elbow_deviation_deg = FromServoDeciDeg(geometry.elbow_zero_deci_deg, geometry.elbow_direction,
                                                        static_cast<double>(angles.elbow_deci_deg));

    // The forearm's angle above horizontal is the shoulder's, turned by however
    // far the elbow deviates from straight.
    const double forearm_deg = shoulder_deg + elbow_deviation_deg;

    const double radius_mm = geometry.shoulder_offset_mm +
                             geometry.shoulder_to_elbow_mm * std::cos(ToRadians(shoulder_deg)) +
                             geometry.elbow_to_tcp_mm * std::cos(ToRadians(forearm_deg));
    const double height_mm = geometry.shoulder_height_mm +
                             geometry.shoulder_to_elbow_mm * std::sin(ToRadians(shoulder_deg)) +
                             geometry.elbow_to_tcp_mm * std::sin(ToRadians(forearm_deg));

    return PickPose{ .x_mm = radius_mm * std::cos(ToRadians(base_deg)),
                     .y_mm = radius_mm * std::sin(ToRadians(base_deg)),
                     .z_mm = height_mm };
}

std::uint32_t MinimumArmDurationMs(const GripperGeometry& geometry, const JointAngles& from,
                                   const JointAngles& to) noexcept {
    const std::uint32_t base_ms = DurationForDelta(AbsoluteDifference(from.base_deci_deg, to.base_deci_deg),
                                                   geometry.base_max_speed_deci_deg_per_sec);
    const std::uint32_t shoulder_ms = DurationForDelta(AbsoluteDifference(from.shoulder_deci_deg, to.shoulder_deci_deg),
                                                       geometry.shoulder_max_speed_deci_deg_per_sec);
    const std::uint32_t elbow_ms = DurationForDelta(AbsoluteDifference(from.elbow_deci_deg, to.elbow_deci_deg),
                                                    geometry.elbow_max_speed_deci_deg_per_sec);

    // The joints move together, so the slowest one sets the pace.
    return std::max({ base_ms, shoulder_ms, elbow_ms });
}

std::uint32_t MinimumClawDurationMs(const GripperGeometry& geometry, std::uint8_t from_percent,
                                    std::uint8_t to_percent) noexcept {
    return DurationForDelta(AbsoluteDifference(from_percent, to_percent), geometry.claw_max_speed_percent_per_sec);
}

std::uint16_t ClampDurationMs(std::uint32_t requested_ms) noexcept {
    const std::uint32_t clamped = std::clamp(requested_ms, static_cast<std::uint32_t>(UART_GRIPPER_DURATION_MS_MIN),
                                             static_cast<std::uint32_t>(UART_GRIPPER_DURATION_MS_MAX));
    return static_cast<std::uint16_t>(clamped);
}

std::string_view ToString(IkStatus status) noexcept {
    switch (status) {
        case IkStatus::kOk:
            return "OK";
        case IkStatus::kUnreachableTooFar:
            return "UNREACHABLE_TOO_FAR";
        case IkStatus::kUnreachableTooClose:
            return "UNREACHABLE_TOO_CLOSE";
        case IkStatus::kJointLimit:
            return "JOINT_LIMIT";
        case IkStatus::kInvalidGeometry:
            return "INVALID_GEOMETRY";
    }
    return "UNKNOWN";
}

}  // namespace logistics::device
