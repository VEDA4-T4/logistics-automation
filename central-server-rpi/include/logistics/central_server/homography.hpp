#pragma once

#include <array>
#include <optional>
#include <string>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

struct HomographyConfig final {
    bool enabled{ false };
    std::array<double, 9> pixel_to_conveyor{ 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    double conveyor_plane_z_mm{};
    double robot_base_x_mm{};
    double robot_base_y_mm{};
    double robot_base_z_mm{};
    double robot_base_yaw_deg{};
    double box_length_mm{};
    double box_width_mm{};
    double box_height_mm{};
    std::string coordinate_frame{ "PI-GRIPPER-01_BASE" };
    int calibration_version{ 1 };

    [[nodiscard]] bool IsValid() const noexcept;
};

struct GripperTarget final {
    double x_mm{};
    double y_mm{};
    double z_mm{};
    double yaw_deg{};
    double box_length_mm{};
    double box_width_mm{};
    double box_height_mm{};
    std::string coordinate_frame;
    int calibration_version{};
};

class HomographyTransformer final {
public:
    explicit HomographyTransformer(HomographyConfig config = {});

    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] std::optional<GripperTarget> Transform(
        const contracts::mqtt::PositionDetectedPayload& position) const noexcept;

private:
    struct Point final {
        double x{};
        double y{};
    };

    [[nodiscard]] std::optional<Point> ToConveyor(const contracts::mqtt::PixelPoint& point) const noexcept;

    HomographyConfig config_;
};

}  // namespace logistics::central_server
