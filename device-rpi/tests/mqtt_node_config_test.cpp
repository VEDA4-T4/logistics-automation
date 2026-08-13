#include "logistics/device/mqtt_node_config.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/mqtt_time.hpp"

namespace {

namespace device = logistics::device;

[[nodiscard]] std::filesystem::path MakeTemporaryConfigPath() {
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("logistics-device-" + std::to_string(timestamp) + ".ini");
}

void TestConfigLoading() {
    const auto path = MakeTemporaryConfigPath();
    {
        std::ofstream output(path);
        assert(output);
        output << R"ini(
[device]
device_id=PI-01
node_name=vision-node-01
ip_address=192.0.2.10

[mqtt]
host=192.0.2.10
port=1884
client_id=PI-01
username=device
password=secret
tls_enabled=false
ca_certificate=
keep_alive_seconds=45
reconnect_min_delay_seconds=2
reconnect_max_delay_seconds=20
clean_session=no
publish_spool_directory=/var/lib/logistics/mqtt-spool
publish_spool_maximum_bytes=16384

[sorting]
default_speed=65

[log_upload]
enabled=true
endpoint_url=https://server.example/api/v1/uploads/logs
bearer_token=device-token
ca_certificate=/etc/logistics/ca.crt
spool_directory=/var/lib/logistics/log-spool
rotate_bytes=1024
rotate_interval_seconds=60
maximum_spool_bytes=8192
request_timeout_seconds=15
maximum_attempts=4
initial_backoff_seconds=2
maximum_backoff_seconds=30
allow_insecure_http=false

[image_upload]
enabled=true
endpoint_url=https://server.example/api/v1/uploads/images
bearer_token=device-token
ca_certificate=/etc/logistics/ca.crt
request_timeout_seconds=20
maximum_attempts=3
initial_backoff_seconds=1
maximum_backoff_seconds=10
allow_insecure_http=false
)ini";
    }

    const auto config = device::LoadMqttNodeConfig(path);
    assert(config.device_id == "PI-01");
    assert(config.node_name == "vision-node-01");
    assert(config.ip_address == "192.0.2.10");
    assert(config.host == "192.0.2.10");
    assert(config.port == 1884);
    assert(config.client_id == "PI-01");
    assert(config.keep_alive_seconds == 45);
    assert(config.reconnect_min_delay_seconds == 2);
    assert(config.reconnect_max_delay_seconds == 20);
    assert(config.sorting_default_speed == 65);
    assert(!config.clean_session);
    assert(config.publish_spool_directory == "/var/lib/logistics/mqtt-spool");
    assert(config.publish_spool_maximum_bytes == 16384);
    assert(config.log_upload_enabled);
    assert(config.log_upload.device_id == "PI-01");
    assert(config.log_upload.endpoint_url == "https://server.example/api/v1/uploads/logs");
    assert(config.log_upload.rotate_bytes == 1024);
    assert(config.log_upload.rotate_interval == std::chrono::seconds(60));
    assert(config.log_upload.maximum_attempts == 4);
    assert(config.image_upload_enabled);
    assert(config.image_upload.endpoint_url == "https://server.example/api/v1/uploads/images");
    assert(config.image_upload.request_timeout == std::chrono::seconds(20));
    assert(config.image_upload.maximum_attempts == 3);
    assert(config.IsValid());

    const std::string timestamp = device::CurrentIso8601Timestamp();
    assert(logistics::contracts::mqtt::IsValidIso8601Timestamp(timestamp));
    const std::string first_session = device::GenerateMessageSessionId();
    const std::string second_session = device::GenerateMessageSessionId();
    assert(first_session != second_session);
    assert(logistics::contracts::mqtt::IsValidTopicLevel(first_session));
    assert(device::MakeMessageId(config.device_id, first_session, 42) == "PI-01-MSG-" + first_session + "-42");
    assert(device::MakeMessageId(config.device_id, first_session, 1) !=
           device::MakeMessageId(config.device_id, second_session, 1));

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestTlsWithoutCaIsRejected() {
    const auto path = MakeTemporaryConfigPath();

    {
        std::ofstream output(path);
        assert(output);
        output << R"ini(
[device]
device_id=PI-TEST
node_name=mqtt-security-test
ip_address=192.0.2.10

[mqtt]
host=192.0.2.10
port=8883
client_id=PI-TEST
username=PI-TEST
password=test-only-password
tls_enabled=true
ca_certificate=
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=true
)ini";
    }

    bool rejected = false;

    try {
        static_cast<void>(device::LoadMqttNodeConfig(path));
    } catch (const device::NodeConfigError&) {
        rejected = true;
    }

    assert(rejected);

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestRegistrationFieldsAreRequired() {
    const auto path = MakeTemporaryConfigPath();
    {
        std::ofstream output(path);
        assert(output);
        output << R"ini(
[device]
device_id=PI-01

[mqtt]
host=192.168.0.10
client_id=PI-01
)ini";
    }

    bool rejected = false;
    try {
        static_cast<void>(device::LoadMqttNodeConfig(path));
    } catch (const device::NodeConfigError&) {
        rejected = true;
    }
    assert(rejected);

    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

int main() {
    TestConfigLoading();
    TestRegistrationFieldsAreRequired();
    TestTlsWithoutCaIsRejected();
    return 0;
}
