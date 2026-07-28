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
)ini");

    const auto config = central_server::LoadServerConfig(path);
    assert(config.database.path == path.parent_path() / "data/server.db");
    assert(config.database.migration_dir == path.parent_path() / "migrations");
    assert(config.storage.image_root == path.parent_path() / "images");
    assert(config.http.upload_root == path.parent_path() / "uploads");
    assert(config.http.tls_certificate.empty());
    assert(config.http.tls_private_key.empty());
    assert(config.http.port == 8081);
    assert(config.process.enabled);
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
    ExpectRejected("section", "[databse]\npath=/tmp/server.db\n");
}

}  // namespace

int main() {
    TestValidConfigAndRelativePaths();
    TestInvalidSettingsAreRejected();
    return 0;
}
