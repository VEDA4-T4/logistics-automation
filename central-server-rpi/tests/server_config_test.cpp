#include "logistics/central_server/server_config.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "logistics/central_server/mqtt_config.hpp"

namespace {

namespace central_server = logistics::central_server;

[[nodiscard]] std::filesystem::path TemporaryPath(std::string_view suffix) {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("logistics-server-config-" + std::to_string(value) + "-" + std::string(suffix) + ".ini");
}

void Write(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path);
    assert(output);
    output << content;
    assert(output.good());
}

void Remove(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestValidConfigAndRelativePaths() {
    const auto path = TemporaryPath("valid");
    Write(path, R"ini(
[database]
path=data/server.db
migration_dir=migrations
busy_timeout_ms=1000
[storage]
image_root=images
log_root=logs
cleanup_interval_hours=12
mqtt_retention_days=10
device_status_retention_days=11
error_retention_days=12
security_retention_days=13
image_retention_days=14
upload_retention_days=15
[http]
enabled=true
port=8081
tls_enabled=false
bearer_token=test-token
tls_certificate=
tls_private_key=
upload_root=uploads
[routing]
qt_client_id=control-center
[process]
enabled=true
server_id=central-server
input_device_id=PI-INPUT-01
vision_device_id=PI-VISION-01
gripper_device_id=PI-GRIPPER-01
sorting_device_id=PI-SORTING-01
line_tracer_device_id=PI-LT-01
line_tracer_enabled=false
line_tracer_initial_position=A
default_destination=3
[homography]
enabled=true
pixel_to_conveyor=2,0,10,0,2,20,0,0,1
conveyor_plane_z_mm=8.5e2
robot_base_x_mm=1250
robot_base_y_mm=430
robot_base_z_mm=0
robot_base_yaw_deg=90
box_length_mm=400
box_width_mm=300
box_height_mm=250
coordinate_frame=PI-GRIPPER-01_BASE
calibration_version=4
)ini");

    const auto config = central_server::LoadServerConfig(path);
    assert(config.database.path == path.parent_path() / "data/server.db");
    assert(config.database.migration_dir == path.parent_path() / "migrations");
    assert(config.storage.image_root == path.parent_path() / "images");
    assert(config.storage.upload_retention_days == 15);
    assert(config.http.upload_root == path.parent_path() / "uploads");
    assert(config.http.tls_certificate.empty());
    assert(config.http.tls_private_key.empty());
    assert(config.http.port == 8081);
    assert(config.process.enabled);
    assert(!config.process.line_tracer_enabled);
    assert(config.process.line_tracer_initial_position == "A");
    assert(config.process.default_destination == "3");
    assert(config.process.homography.enabled);
    assert(config.process.homography.pixel_to_conveyor[0] == 2.0);
    assert(config.process.homography.pixel_to_conveyor[2] == 10.0);
    assert(config.process.homography.conveyor_plane_z_mm == 850.0);
    assert(config.process.homography.box_height_mm == 250.0);
    assert(config.process.homography.calibration_version == 4);
    Remove(path);
}

void ExpectRejected(std::string_view name, std::string_view content) {
    const auto path = TemporaryPath(name);
    Write(path, content);
    bool rejected = false;
    try {
        static_cast<void>(central_server::LoadServerConfig(path));
    } catch (const central_server::ConfigError&) {
        rejected = true;
    }
    assert(rejected);
    Remove(path);
}

void TestInvalidSettingsAreRejected() {
    ExpectRejected("unknown-key", "[database]\npat=/tmp/server.db\n");
    ExpectRejected("unknown-storage-key", "[storage]\nlog_rooot=/tmp/logs\n");
    ExpectRejected("duplicate", "[http]\nenabled=false\nenabled=true\n");
    ExpectRejected("port", "[http]\nenabled=false\nport=70000\n");
    ExpectRejected("token", "[http]\nenabled=true\nbearer_token=\n");
    ExpectRejected("tls-empty",
                   "[http]\nenabled=true\nbearer_token=token\ntls_enabled=true\n"
                   "tls_certificate=\ntls_private_key=\n");
    ExpectRejected("tls-files",
                   "[http]\nenabled=true\nbearer_token=token\ntls_enabled=true\n"
                   "tls_certificate=missing.crt\ntls_private_key=missing.key\n");
    ExpectRejected("line-tracer-position", "[process]\nline_tracer_initial_position=D\n");
    ExpectRejected("default-destination", "[process]\ndefault_destination=bad/destination\n");
    ExpectRejected("homography-matrix", "[homography]\nenabled=true\npixel_to_conveyor=1,0,0\n");
    ExpectRejected("homography-number", "[homography]\nconveyor_plane_z_mm=850mm\n");
    ExpectRejected("homography-singular",
                   "[homography]\nenabled=true\npixel_to_conveyor=1,0,0,0,0,0,0,0,1\n"
                   "box_length_mm=400\nbox_width_mm=300\nbox_height_mm=250\n");
    ExpectRejected("homography-box",
                   "[homography]\nenabled=true\npixel_to_conveyor=1,0,0,0,1,0,0,0,1\n"
                   "box_length_mm=200\nbox_width_mm=300\nbox_height_mm=250\n");
    ExpectRejected("section", "[databse]\npath=/tmp/server.db\n");
}

}  // namespace

int main() {
    TestValidConfigAndRelativePaths();
    TestInvalidSettingsAreRejected();
    return 0;
}
