#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "logistics/contracts/uart/gripper_commands.h"

namespace logistics::device {

/*
 * Cartesian pick/place target as the central server sends it.
 *
 * Coordinates are millimetres in the arm's own frame:
 *
 *   origin  the base rotation axis, at the height of the mounting plate
 *   +Z      up
 *   +X      the direction the arm points when the base servo sits at
 *           GripperGeometry::base_zero_deci_deg
 *   +Y      90 degrees counter-clockwise from +X, seen from above
 *
 * The arm has three degrees of freedom (base yaw, shoulder pitch, elbow pitch)
 * and no wrist roll, so the claw's grasp axis is rigidly tied to the base angle.
 * Because the base angle is fully determined by the target's (x, y), box
 * orientation cannot be commanded independently and is deliberately not part of
 * this struct: the server may compute a box yaw for its own bookkeeping, but the
 * node ignores it. Honouring it would require a fourth joint.
 */
struct PickPose {
    double x_mm{};
    double y_mm{};
    double z_mm{};
};

/* Joint targets in the units the UART contract uses: unsigned deci-degrees. */
struct JointAngles {
    std::uint16_t base_deci_deg{};
    std::uint16_t shoulder_deci_deg{};
    std::uint16_t elbow_deci_deg{};
};

/*
 * Mechanical description of the assembled arm.
 *
 * Two groups of values live here for different reasons. The link lengths and
 * zero references describe geometry that only this node knows about. The joint
 * limits and speed limits below them are a deliberate *mirror* of the firmware's
 * own values in stm32/gripper-controller/Application/Inc/gripper_calibration.h.
 *
 * The mirror matters twice over:
 *
 *   - Limits: the UART contract accepts 0.0 to 180.0 degrees on every joint, but
 *     the firmware applies the narrower ranges below and answers a target outside
 *     them with INVALID_PAYLOAD. Checking here turns that into a specific MQTT
 *     error naming the joint, instead of a generic controller rejection.
 *   - Speeds: the firmware does not reject a too-short duration, it silently
 *     stretches the motion to max(requested, its own minimum). A node that did
 *     not reproduce that formula would time out waiting for a motion that is
 *     still running correctly.
 *
 * Both groups are configurable so that tuning the firmware against the assembled
 * arm does not require rebuilding this node, but they must be kept in step with
 * the firmware header.
 */
struct GripperGeometry {
    /*
     * Link lengths, measured between joint rotation axes.
     *
     * Ruler measurement on the assembled arm (2026-07-30), which supersedes an
     * earlier guess taken from the seller's product listing: the listing gave no
     * dimensioned drawing and the number derived from it (135 mm) was well off
     * from what the physical unit turned out to be.
     *
     * Note what the two links imply together: with the grasp centre 175 mm out
     * (below) the arm reaches 260 mm at full stretch but cannot fold tighter than
     * 175 - 85 = 90 mm from the shoulder pivot, so near targets are as
     * unreachable as far ones.
     */
    double shoulder_to_elbow_mm{ 85.0 };

    /*
     * Elbow axis to the claw's grasp centre.
     *
     * The arm has no wrist joint -- the bracket between the forearm and the claw
     * is rigid -- so this is the forearm (110 mm, elbow axis to the claw mount)
     * plus the claw itself (65 mm, mount to tip) measured as one straight run:
     * 175 mm. Unlike the link above, both halves of this number came off the
     * ruler directly.
     */
    double elbow_to_tcp_mm{ 175.0 };

    /*
     * Height of the shoulder pivot above the mounting plate, and its horizontal
     * offset from the base rotation axis. Server Z coordinates are measured from
     * that same plate.
     *
     * 20 mm is the measured height of the base stand below the shoulder servo.
     * The horizontal offset has not been measured and is assumed zero; these
     * frames typically stack the base and shoulder axes directly, but confirm it
     * if targets near the base axis solve with a visibly wrong yaw.
     */
    double shoulder_height_mm{ 20.0 };
    double shoulder_offset_mm{ 0.0 };

    /*
     * Servo zero references and travel directions.
     *
     *   servo_angle = zero + direction * geometric_angle
     *
     * so a joint whose linkage is mirrored only needs its direction flipped in
     * the INI rather than a code change. The geometric angles are: base yaw
     * measured from +X, shoulder pitch measured up from horizontal, and elbow
     * measured as the deviation from a fully extended arm.
     */
    double base_zero_deci_deg{ 900.0 };
    double shoulder_zero_deci_deg{ 900.0 };
    /*
     * The elbow's zero sits at the top of its travel rather than mid-range,
     * because its geometric angle only ever goes one way: a fully extended arm is
     * the maximum and folding can only reduce it. Referencing it to 90.0 degrees
     * like the other two would throw away half the servo's range on postures that
     * would hyperextend the joint.
     */
    double elbow_zero_deci_deg{ 1800.0 };
    int base_direction{ 1 };
    int shoulder_direction{ 1 };
    int elbow_direction{ 1 };

    // Mirror of GRIPPER_*_{MIN,MAX}_ANGLE_DECI_DEG.
    std::uint16_t base_min_deci_deg{ 100U };
    std::uint16_t base_max_deci_deg{ 1800U };
    std::uint16_t shoulder_min_deci_deg{ 300U };
    std::uint16_t shoulder_max_deci_deg{ 1500U };
    std::uint16_t elbow_min_deci_deg{ 300U };
    std::uint16_t elbow_max_deci_deg{ 1800U };

    // Mirror of GRIPPER_*_MAX_SPEED_DECI_DEG_PER_SEC and
    // GRIPPER_CLAW_MAX_SPEED_PERCENT_PER_SEC.
    std::uint16_t base_max_speed_deci_deg_per_sec{ 300U };
    std::uint16_t shoulder_max_speed_deci_deg_per_sec{ 120U };
    std::uint16_t elbow_max_speed_deci_deg_per_sec{ 200U };
    std::uint16_t claw_max_speed_percent_per_sec{ 40U };

    // Mirror of GRIPPER_HOME_POSITION_PERCENT. Needed because a completed HOME is
    // the node's anchor for claw travel arithmetic as well as arm travel.
    std::uint8_t home_claw_percent{ 65U };

    [[nodiscard]] bool IsValid() const noexcept;

    // Joint angles the arm is commanded to when it homes. Used as the starting
    // point for duration arithmetic before the first status read.
    [[nodiscard]] JointAngles HomeAngles() const noexcept;

    [[nodiscard]] bool AnglesWithinLimits(const JointAngles& angles) const noexcept;
};

enum class IkStatus {
    kOk,
    // The target is outside the annulus the two links can span. Splitting the
    // two directions apart is worth it operationally: too far usually means a
    // bad calibration or a detection on the far conveyor, while too close means
    // the arm would have to fold through itself.
    kUnreachableTooFar,
    kUnreachableTooClose,
    // A solution exists geometrically but at least one joint would have to leave
    // the range the firmware accepts.
    kJointLimit,
    // Link lengths or zero references are unusable.
    kInvalidGeometry,
};

struct IkSolution {
    IkStatus status{ IkStatus::kInvalidGeometry };
    JointAngles angles{};
    // Which joint blocked the solution, set only for kJointLimit so the error
    // message can name it.
    std::string_view blocking_joint{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == IkStatus::kOk;
    }
};

/*
 * Solves the 3-DOF arm for a Cartesian target.
 *
 * The base yaw follows straight from atan2(y, x); the shoulder and elbow are a
 * planar two-link problem in the vertical plane that contains the arm. Of the two
 * mathematical solutions the elbow-up one is chosen, because elbow-down drives
 * the forearm through the mounting plate for any target the conveyor can present.
 */
[[nodiscard]] IkSolution SolveInverseKinematics(const GripperGeometry& geometry, const PickPose& pose) noexcept;

/* Forward kinematics, used by the tests to check a solution round-trips. */
[[nodiscard]] std::optional<PickPose> SolveForwardKinematics(const GripperGeometry& geometry,
                                                             const JointAngles& angles) noexcept;

/*
 * Shortest duration the firmware will accept for an arm move without stretching
 * it, i.e. the largest per-joint travel time under the mirrored speed limits.
 *
 * This reproduces gripper_control_arm_minimum_duration() in the firmware,
 * including its round-up, so the two sides agree on when a motion should have
 * finished.
 */
[[nodiscard]] std::uint32_t MinimumArmDurationMs(const GripperGeometry& geometry, const JointAngles& from,
                                                 const JointAngles& to) noexcept;

/* Same, for gripper_control_claw_minimum_duration(). */
[[nodiscard]] std::uint32_t MinimumClawDurationMs(const GripperGeometry& geometry, std::uint8_t from_percent,
                                                  std::uint8_t to_percent) noexcept;

/*
 * Duration to put on the wire: the configured nominal time, raised to whatever
 * the speed limits demand, then held inside the contract's range.
 *
 * The value can still be below the returned minimum when the contract's 10 s
 * ceiling bites, which is why callers time out against the raw minimum rather
 * than against this.
 */
[[nodiscard]] std::uint16_t ClampDurationMs(std::uint32_t requested_ms) noexcept;

[[nodiscard]] std::string_view ToString(IkStatus status) noexcept;

}  // namespace logistics::device
