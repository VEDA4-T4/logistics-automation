#include "logistics/central_server/homography.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {
namespace {

constexpr double kMinimumDenominator = 1.0e-9;
constexpr double kMinimumNormalizedDeterminant = 1.0e-12;
constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] bool IsFinite(const double value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] double DegreesToRadians(const double degrees) noexcept {
    return degrees * kPi / 180.0;
}

[[nodiscard]] double RadiansToDegrees(const double radians) noexcept {
    return radians * 180.0 / kPi;
}

[[nodiscard]] double NormalizeHalfTurn(double degrees) noexcept {
    while (degrees >= 90.0) {
        degrees -= 180.0;
    }
    while (degrees < -90.0) {
        degrees += 180.0;
    }
    return degrees;
}

[[nodiscard]] double SquaredDistance(const double lhs_x, const double lhs_y, const double rhs_x,
                                     const double rhs_y) noexcept {
    const double x = rhs_x - lhs_x;
    const double y = rhs_y - lhs_y;
    return x * x + y * y;
}

[[nodiscard]] bool IsScaleIndependentInvertible(const std::array<double, 9>& matrix) noexcept {
    double coefficient_scale{};
    for (const double value : matrix) {
        coefficient_scale = std::max(coefficient_scale, std::abs(value));
    }
    if (coefficient_scale == 0.0) {
        return false;
    }

    std::array<double, 9> normalized{};
    std::transform(matrix.begin(), matrix.end(), normalized.begin(),
                   [coefficient_scale](const double value) { return value / coefficient_scale; });
    const double determinant = normalized[0] * (normalized[4] * normalized[8] - normalized[5] * normalized[7]) -
                               normalized[1] * (normalized[3] * normalized[8] - normalized[5] * normalized[6]) +
                               normalized[2] * (normalized[3] * normalized[7] - normalized[4] * normalized[6]);
    return std::abs(determinant) > kMinimumNormalizedDeterminant;
}

}  // namespace

bool HomographyConfig::IsValid() const noexcept {
    const bool finite_matrix = std::all_of(pixel_to_conveyor.begin(), pixel_to_conveyor.end(),
                                           [](const double value) { return IsFinite(value); });
    return finite_matrix && IsScaleIndependentInvertible(pixel_to_conveyor) && IsFinite(conveyor_plane_z_mm) &&
           IsFinite(robot_base_x_mm) && IsFinite(robot_base_y_mm) && IsFinite(robot_base_z_mm) &&
           IsFinite(robot_base_yaw_deg) && IsFinite(box_length_mm) && IsFinite(box_width_mm) &&
           IsFinite(box_height_mm) && box_length_mm >= box_width_mm && box_width_mm > 0.0 && box_height_mm > 0.0 &&
           contracts::mqtt::IsValidTopicLevel(coordinate_frame) && calibration_version > 0;
}

HomographyTransformer::HomographyTransformer(HomographyConfig config) : config_(std::move(config)) {
    if (config_.enabled && !config_.IsValid()) {
        throw std::invalid_argument("invalid homography configuration");
    }
}

bool HomographyTransformer::Enabled() const noexcept {
    return config_.enabled;
}

std::optional<GripperTarget> HomographyTransformer::Transform(
    const contracts::mqtt::PositionDetectedPayload& position) const noexcept {
    if (!config_.enabled || !position.box_corners.has_value()) {
        return std::nullopt;
    }

    std::array<Point, 4> conveyor_corners{};
    for (std::size_t index = 0; index < conveyor_corners.size(); ++index) {
        const auto transformed = ToConveyor((*position.box_corners)[index]);
        if (!transformed.has_value()) {
            return std::nullopt;
        }
        conveyor_corners[index] = *transformed;
    }

    Point center{};
    for (const auto& point : conveyor_corners) {
        center.x += point.x;
        center.y += point.y;
    }
    center.x /= static_cast<double>(conveyor_corners.size());
    center.y /= static_cast<double>(conveyor_corners.size());

    const Point edge_one{
        .x = conveyor_corners[1].x - conveyor_corners[0].x,
        .y = conveyor_corners[1].y - conveyor_corners[0].y,
    };
    const Point edge_two{
        .x = conveyor_corners[2].x - conveyor_corners[1].x,
        .y = conveyor_corners[2].y - conveyor_corners[1].y,
    };
    const Point box_axis =
        SquaredDistance(conveyor_corners[0].x, conveyor_corners[0].y, conveyor_corners[1].x, conveyor_corners[1].y) >=
                SquaredDistance(conveyor_corners[1].x, conveyor_corners[1].y, conveyor_corners[2].x,
                                conveyor_corners[2].y)
            ? edge_one
            : edge_two;
    if (std::hypot(box_axis.x, box_axis.y) <= kMinimumDenominator) {
        return std::nullopt;
    }

    const double robot_yaw_radians = DegreesToRadians(config_.robot_base_yaw_deg);
    const double delta_x = center.x - config_.robot_base_x_mm;
    const double delta_y = center.y - config_.robot_base_y_mm;
    const double cosine = std::cos(robot_yaw_radians);
    const double sine = std::sin(robot_yaw_radians);
    const double box_yaw_degrees = RadiansToDegrees(std::atan2(box_axis.y, box_axis.x));

    return GripperTarget{
        .x_mm = cosine * delta_x + sine * delta_y,
        .y_mm = -sine * delta_x + cosine * delta_y,
        .z_mm = config_.conveyor_plane_z_mm + config_.box_height_mm - config_.robot_base_z_mm,
        .yaw_deg = NormalizeHalfTurn(box_yaw_degrees - config_.robot_base_yaw_deg),
        .box_length_mm = config_.box_length_mm,
        .box_width_mm = config_.box_width_mm,
        .box_height_mm = config_.box_height_mm,
        .coordinate_frame = config_.coordinate_frame,
        .calibration_version = config_.calibration_version,
    };
}

std::optional<HomographyTransformer::Point> HomographyTransformer::ToConveyor(
    const contracts::mqtt::PixelPoint& point) const noexcept {
    const auto& h = config_.pixel_to_conveyor;
    const double denominator = h[6] * point.x + h[7] * point.y + h[8];
    if (!IsFinite(denominator) || std::abs(denominator) <= kMinimumDenominator) {
        return std::nullopt;
    }

    const Point transformed{
        .x = (h[0] * point.x + h[1] * point.y + h[2]) / denominator,
        .y = (h[3] * point.x + h[4] * point.y + h[5]) / denominator,
    };
    if (!IsFinite(transformed.x) || !IsFinite(transformed.y)) {
        return std::nullopt;
    }
    return transformed;
}

}  // namespace logistics::central_server
