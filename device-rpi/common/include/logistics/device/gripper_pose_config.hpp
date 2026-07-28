#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "logistics/contracts/uart/gripper_commands.h"

namespace logistics::device {

/*
 * Taught joint poses for the gripper arm.
 *
 * The STM32 only accepts calibrated joint targets, so the Raspberry Pi owns
 * every decision about where the arm goes. The current central-server command
 * carries a work ID and a destination but no coordinates, so the poses below
 * are taught once against the assembled arm and stored in the node INI.
 *
 * Vision offsets, when the server starts forwarding them, are applied on top of
 * the taught pick pose as a base-joint correction; see ApplyBaseOffset. That
 * keeps the taught-pose flow working today without blocking a coordinate-driven
 * flow later.
 */
struct GripperPose {
    std::uint16_t base_deci_deg{ 900U };
    std::uint16_t shoulder_deci_deg{ 900U };
    std::uint16_t elbow_deci_deg{ 900U };

    [[nodiscard]] bool IsValid() const noexcept {
        return uart_gripper_angle_is_valid(base_deci_deg) != 0U &&
               uart_gripper_angle_is_valid(shoulder_deci_deg) != 0U &&
               uart_gripper_angle_is_valid(elbow_deci_deg) != 0U;
    }
};

struct GripperPoseConfig {
    // Waypoints of one pick-and-place cycle. "approach" poses sit directly above
    // their target so the claw descends vertically instead of sweeping into the
    // conveyor side wall.
    GripperPose home{ 900U, 900U, 900U };
    GripperPose pick_approach{ 600U, 700U, 900U };
    GripperPose pick{ 600U, 1100U, 700U };
    GripperPose place_approach{ 1200U, 700U, 900U };
    GripperPose place{ 1200U, 1100U, 700U };

    // Claw travel. 0 is fully closed and 100 is fully open after calibration.
    std::uint8_t open_position_percent{ 100U };
    std::uint8_t closed_position_percent{ 30U };

    // Interpolation time the controller uses for each motion class.
    std::uint16_t arm_duration_ms{ 1500U };
    std::uint16_t claw_duration_ms{ 600U };

    /*
     * Base-joint correction applied per pixel of vision offset, in deci-degrees.
     * Zero (the default) disables the correction entirely, which is the correct
     * behaviour until the arm and camera are calibrated together.
     */
    double base_deci_deg_per_pixel{ 0.0 };

    // Largest correction accepted before the offset is treated as implausible.
    std::uint16_t max_base_correction_deci_deg{ 150U };

    [[nodiscard]] bool IsValid() const noexcept;

    /*
     * Returns the pick pose shifted by a vision offset, clamped to the
     * configured correction limit and to the contract's angle range.
     */
    [[nodiscard]] GripperPose PickPoseForOffset(std::int32_t offset_x_pixels) const noexcept;
};

class GripperConfigError final : public std::runtime_error {
public:
    explicit GripperConfigError(const std::string& message) : std::runtime_error(message) {}
};

/*
 * Reads the [gripper] section of a node INI.
 *
 * LoadMqttNodeConfig silently skips sections it does not know, so the gripper
 * poses live in the same file as the device and MQTT settings. A file with no
 * [gripper] section yields the defaults above.
 */
[[nodiscard]] GripperPoseConfig LoadGripperPoseConfig(const std::filesystem::path& path);

/*
 * Parses the same content from memory. Exposed so the poses can be unit tested
 * on a developer host without touching the filesystem.
 */
[[nodiscard]] GripperPoseConfig ParseGripperPoseConfig(std::string_view contents, std::string_view origin = "<memory>");

}  // namespace logistics::device
