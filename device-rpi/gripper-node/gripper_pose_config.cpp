#include "logistics/device/gripper_pose_config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace logistics::device {
namespace {

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

[[noreturn]] void ThrowLineError(std::string_view origin, std::size_t line_number, std::string_view detail) {
    throw GripperConfigError(std::string(origin) + ":" + std::to_string(line_number) + ": " + std::string(detail));
}

template <typename Integer>
[[nodiscard]] Integer ParseInteger(std::string_view origin, std::size_t line_number, std::string_view key,
                                   std::string_view value, Integer minimum, Integer maximum) {
    Integer parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < minimum || parsed > maximum) {
        ThrowLineError(origin, line_number, std::string(key) + " is outside the allowed range");
    }
    return parsed;
}

[[nodiscard]] double ParseScale(std::string_view origin, std::size_t line_number, std::string_view key,
                                std::string_view value) {
    // from_chars for floating point is not available in every toolchain used on
    // the Raspberry Pi images, so the scale goes through istringstream instead.
    std::istringstream stream{ std::string(value) };
    stream.imbue(std::locale::classic());
    double parsed{};
    stream >> parsed;
    if (stream.fail() || !stream.eof() || !std::isfinite(parsed)) {
        ThrowLineError(origin, line_number, std::string(key) + " must be a decimal number");
    }
    return parsed;
}

/*
 * Poses are written as "base,shoulder,elbow" in deci-degrees so one taught
 * waypoint stays on one line and is easy to compare against a measured arm.
 */
[[nodiscard]] GripperPose ParsePose(std::string_view origin, std::size_t line_number, std::string_view key,
                                    std::string_view value) {
    std::array<std::uint16_t, 3U> angles{};
    std::size_t parsed_count = 0U;
    std::size_t start = 0U;

    while (start <= value.size() && parsed_count < angles.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string_view field =
            Trim(value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        angles[parsed_count] =
            ParseInteger<std::uint16_t>(origin, line_number, key, field, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
        ++parsed_count;
        if (comma == std::string_view::npos) {
            start = value.size() + 1U;
            break;
        }
        start = comma + 1U;
    }

    if (parsed_count != angles.size() || start <= value.size()) {
        ThrowLineError(origin, line_number, std::string(key) + " must be base,shoulder,elbow in deci-degrees");
    }
    return GripperPose{ angles[0], angles[1], angles[2] };
}

void AssignGripperValue(GripperPoseConfig& config, std::string_view origin, std::size_t line_number,
                        std::string_view key, std::string_view value) {
    if (key == "home_pose") {
        config.home = ParsePose(origin, line_number, key, value);
    } else if (key == "pick_approach_pose") {
        config.pick_approach = ParsePose(origin, line_number, key, value);
    } else if (key == "pick_pose") {
        config.pick = ParsePose(origin, line_number, key, value);
    } else if (key == "place_approach_pose") {
        config.place_approach = ParsePose(origin, line_number, key, value);
    } else if (key == "place_pose") {
        config.place = ParsePose(origin, line_number, key, value);
    } else if (key == "open_position_percent") {
        config.open_position_percent =
            ParseInteger<std::uint8_t>(origin, line_number, key, value, 0U, UART_GRIPPER_POSITION_MAX);
    } else if (key == "closed_position_percent") {
        config.closed_position_percent =
            ParseInteger<std::uint8_t>(origin, line_number, key, value, 0U, UART_GRIPPER_POSITION_MAX);
    } else if (key == "arm_duration_ms") {
        config.arm_duration_ms = ParseInteger<std::uint16_t>(
            origin, line_number, key, value, UART_GRIPPER_DURATION_MS_MIN, UART_GRIPPER_DURATION_MS_MAX);
    } else if (key == "claw_duration_ms") {
        config.claw_duration_ms = ParseInteger<std::uint16_t>(
            origin, line_number, key, value, UART_GRIPPER_DURATION_MS_MIN, UART_GRIPPER_DURATION_MS_MAX);
    } else if (key == "base_deci_deg_per_pixel") {
        config.base_deci_deg_per_pixel = ParseScale(origin, line_number, key, value);
    } else if (key == "max_base_correction_deci_deg") {
        config.max_base_correction_deci_deg =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
    } else if (key == "link_shoulder_to_elbow_mm") {
        config.geometry.shoulder_to_elbow_mm = ParseScale(origin, line_number, key, value);
    } else if (key == "link_elbow_to_tcp_mm") {
        config.geometry.elbow_to_tcp_mm = ParseScale(origin, line_number, key, value);
    } else if (key == "shoulder_height_mm") {
        config.geometry.shoulder_height_mm = ParseScale(origin, line_number, key, value);
    } else if (key == "shoulder_offset_mm") {
        config.geometry.shoulder_offset_mm = ParseScale(origin, line_number, key, value);
    } else if (key == "base_zero_deci_deg") {
        config.geometry.base_zero_deci_deg = ParseScale(origin, line_number, key, value);
    } else if (key == "shoulder_zero_deci_deg") {
        config.geometry.shoulder_zero_deci_deg = ParseScale(origin, line_number, key, value);
    } else if (key == "elbow_zero_deci_deg") {
        config.geometry.elbow_zero_deci_deg = ParseScale(origin, line_number, key, value);
    } else if (key == "base_direction") {
        config.geometry.base_direction = ParseInteger<int>(origin, line_number, key, value, -1, 1);
    } else if (key == "shoulder_direction") {
        config.geometry.shoulder_direction = ParseInteger<int>(origin, line_number, key, value, -1, 1);
    } else if (key == "elbow_direction") {
        config.geometry.elbow_direction = ParseInteger<int>(origin, line_number, key, value, -1, 1);
    } else if (key == "base_min_deci_deg") {
        config.geometry.base_min_deci_deg =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
    } else if (key == "base_max_deci_deg") {
        config.geometry.base_max_deci_deg =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
    } else if (key == "shoulder_min_deci_deg") {
        config.geometry.shoulder_min_deci_deg =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
    } else if (key == "shoulder_max_deci_deg") {
        config.geometry.shoulder_max_deci_deg =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
    } else if (key == "elbow_min_deci_deg") {
        config.geometry.elbow_min_deci_deg =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
    } else if (key == "elbow_max_deci_deg") {
        config.geometry.elbow_max_deci_deg =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 0U, UART_GRIPPER_ANGLE_DECI_DEG_MAX);
    } else if (key == "base_max_speed_deci_deg_per_sec") {
        config.geometry.base_max_speed_deci_deg_per_sec =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 1U, 18000U);
    } else if (key == "shoulder_max_speed_deci_deg_per_sec") {
        config.geometry.shoulder_max_speed_deci_deg_per_sec =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 1U, 18000U);
    } else if (key == "elbow_max_speed_deci_deg_per_sec") {
        config.geometry.elbow_max_speed_deci_deg_per_sec =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 1U, 18000U);
    } else if (key == "claw_max_speed_percent_per_sec") {
        config.geometry.claw_max_speed_percent_per_sec =
            ParseInteger<std::uint16_t>(origin, line_number, key, value, 1U, 1000U);
    } else if (key == "home_claw_percent") {
        config.geometry.home_claw_percent =
            ParseInteger<std::uint8_t>(origin, line_number, key, value, 0U, UART_GRIPPER_POSITION_MAX);
    } else if (key == "approach_height_mm") {
        config.approach_height_mm = ParseScale(origin, line_number, key, value);
    } else if (key == "min_target_z_mm") {
        config.min_target_z_mm = ParseScale(origin, line_number, key, value);
    } else {
        ThrowLineError(origin, line_number, "unknown [gripper] setting: " + std::string(key));
    }
}

}  // namespace

bool GripperPoseConfig::IsValid() const noexcept {
    return home.IsValid() && pick_approach.IsValid() && pick.IsValid() && place_approach.IsValid() && place.IsValid() &&
           uart_gripper_position_is_valid(open_position_percent) != 0U &&
           uart_gripper_position_is_valid(closed_position_percent) != 0U &&
           uart_gripper_duration_is_valid(arm_duration_ms) != 0U &&
           uart_gripper_duration_is_valid(claw_duration_ms) != 0U &&
           // A claw that closes no further than it opens would never grip a box.
           closed_position_percent < open_position_percent && std::isfinite(base_deci_deg_per_pixel) &&
           geometry.IsValid() &&
           // A non-positive clearance would make the approach pose the target
           // itself, so the claw would sweep in sideways instead of descending.
           std::isfinite(approach_height_mm) && approach_height_mm > 0.0 && std::isfinite(min_target_z_mm);
}

PickPose GripperPoseConfig::ApproachAbove(const PickPose& target) const noexcept {
    return PickPose{ .x_mm = target.x_mm, .y_mm = target.y_mm, .z_mm = target.z_mm + approach_height_mm };
}

GripperPose GripperPoseConfig::PickPoseForOffset(std::int32_t offset_x_pixels) const noexcept {
    GripperPose pose = pick;
    if (base_deci_deg_per_pixel == 0.0 || offset_x_pixels == 0) {
        return pose;
    }

    const double requested = static_cast<double>(offset_x_pixels) * base_deci_deg_per_pixel;
    const double limit = static_cast<double>(max_base_correction_deci_deg);
    const double clamped = std::clamp(requested, -limit, limit);
    const double corrected = static_cast<double>(pose.base_deci_deg) + clamped;

    // The controller clamps to its own mechanical limits as well, but keeping the
    // value inside the contract range here means the frame is never rejected
    // outright and the reason for the clamp stays visible on this side.
    pose.base_deci_deg =
        static_cast<std::uint16_t>(std::clamp(corrected, 0.0, static_cast<double>(UART_GRIPPER_ANGLE_DECI_DEG_MAX)));
    return pose;
}

GripperPoseConfig ParseGripperPoseConfig(std::string_view contents, std::string_view origin) {
    GripperPoseConfig config;
    std::istringstream input{ std::string(contents) };
    std::string section;
    std::string line;
    std::size_t line_number{};
    std::unordered_set<std::string> assigned_keys;

    while (std::getline(input, line)) {
        ++line_number;
        std::string_view text = Trim(line);
        if (line_number == 1U && text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
            text = Trim(text);
        }
        if (text.empty() || text.front() == '#' || text.front() == ';') {
            continue;
        }
        if (text.front() == '[' && text.back() == ']') {
            section = Trim(text.substr(1, text.size() - 2));
            continue;
        }
        // Every other section belongs to LoadMqttNodeConfig; skipping them keeps
        // both loaders reading the same file without fighting over unknown keys.
        if (section != "gripper") {
            continue;
        }

        const auto delimiter = text.find('=');
        if (delimiter == std::string_view::npos) {
            ThrowLineError(origin, line_number, "expected key=value");
        }
        const std::string_view key = Trim(text.substr(0, delimiter));
        const std::string_view value = Trim(text.substr(delimiter + 1));
        if (key.empty() || !assigned_keys.emplace(std::string(key)).second) {
            ThrowLineError(origin, line_number, "empty or duplicate setting: " + std::string(key));
        }
        AssignGripperValue(config, origin, line_number, key, value);
    }

    if (!config.IsValid()) {
        throw GripperConfigError("invalid [gripper] configuration in " + std::string(origin));
    }
    return config;
}

GripperPoseConfig LoadGripperPoseConfig(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw GripperConfigError("unable to open gripper configuration: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return ParseGripperPoseConfig(buffer.str(), path.string());
}

}  // namespace logistics::device
