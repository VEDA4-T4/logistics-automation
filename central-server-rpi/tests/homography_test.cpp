#include "logistics/central_server/homography.hpp"

#include <cassert>
#include <cmath>

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

[[nodiscard]] bool Near(const double actual, const double expected) {
    return std::abs(actual - expected) < 1.0e-6;
}

mqtt::PositionDetectedPayload Position() {
    return {
        .work_id = "d8e9b2be-bfc0-471c-9000-590123412345",
        .box_x = 100,
        .box_y = 50,
        .box_width = 200,
        .box_height = 100,
        .center_x = 200,
        .center_y = 100,
        .offset_x = 0,
        .offset_y = 0,
        .position_status = "DETECTED",
        .box_corners =
            std::array{
                mqtt::PixelPoint{ .x = 100.0, .y = 50.0 },
                mqtt::PixelPoint{ .x = 300.0, .y = 50.0 },
                mqtt::PixelPoint{ .x = 300.0, .y = 150.0 },
                mqtt::PixelPoint{ .x = 100.0, .y = 150.0 },
            },
    };
}

central_server::HomographyConfig Config() {
    return {
        .enabled = true,
        .pixel_to_conveyor = { 2.0, 0.0, 10.0, 0.0, 2.0, 20.0, 0.0, 0.0, 1.0 },
        .conveyor_plane_z_mm = 850.0,
        .robot_base_x_mm = 100.0,
        .robot_base_y_mm = 50.0,
        .robot_base_z_mm = 10.0,
        .robot_base_yaw_deg = 90.0,
        .box_length_mm = 400.0,
        .box_width_mm = 200.0,
        .box_height_mm = 250.0,
        .coordinate_frame = "PI-GRIPPER-01_BASE",
        .calibration_version = 7,
    };
}

void TestPixelCornersBecomeRobotRelativePose() {
    const central_server::HomographyTransformer transformer(Config());
    const auto target = transformer.Transform(Position());

    assert(target.has_value());
    assert(Near(target->x_mm, 170.0));
    assert(Near(target->y_mm, -310.0));
    assert(Near(target->z_mm, 1090.0));
    assert(Near(target->yaw_deg, -90.0));
    assert(target->coordinate_frame == "PI-GRIPPER-01_BASE");
    assert(target->calibration_version == 7);
}

void TestRotatedBoxProducesRobotRelativeYaw() {
    central_server::HomographyConfig config = Config();
    config.pixel_to_conveyor = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    config.robot_base_yaw_deg = 0.0;
    central_server::HomographyTransformer transformer(config);
    auto position = Position();
    position.box_corners = std::array{
        mqtt::PixelPoint{ .x = 100.0, .y = 100.0 },
        mqtt::PixelPoint{ .x = 300.0, .y = 300.0 },
        mqtt::PixelPoint{ .x = 250.0, .y = 350.0 },
        mqtt::PixelPoint{ .x = 50.0, .y = 150.0 },
    };

    const auto target = transformer.Transform(position);
    assert(target.has_value());
    assert(Near(target->yaw_deg, 45.0));
}

void TestCornersAreRequiredWhenEnabled() {
    const central_server::HomographyTransformer transformer(Config());
    auto position = Position();
    position.box_corners.reset();
    assert(!transformer.Transform(position).has_value());
}

void TestEquivalentScaledMatrixRemainsValid() {
    central_server::HomographyConfig config = Config();
    for (double& coefficient : config.pixel_to_conveyor) {
        coefficient *= 1.0e-12;
    }

    assert(config.IsValid());
    const central_server::HomographyTransformer transformer(config);
    const auto target = transformer.Transform(Position());
    assert(target.has_value());
    assert(Near(target->x_mm, 170.0));
    assert(Near(target->y_mm, -310.0));
}

}  // namespace

int main() {
    TestPixelCornersBecomeRobotRelativePose();
    TestRotatedBoxProducesRobotRelativeYaw();
    TestCornersAreRequiredWhenEnabled();
    TestEquivalentScaledMatrixRemainsValid();
    return 0;
}
