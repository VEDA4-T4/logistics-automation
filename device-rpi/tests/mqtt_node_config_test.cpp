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

[mqtt]
host=192.168.0.10
port=1884
client_id=PI-01
username=device
password=secret
keep_alive_seconds=45
reconnect_min_delay_seconds=2
reconnect_max_delay_seconds=20
clean_session=yes
)ini";
    }

    const auto config = device::LoadMqttNodeConfig(path);
    assert(config.device_id == "PI-01");
    assert(config.host == "192.168.0.10");
    assert(config.port == 1884);
    assert(config.client_id == "PI-01");
    assert(config.keep_alive_seconds == 45);
    assert(config.reconnect_min_delay_seconds == 2);
    assert(config.reconnect_max_delay_seconds == 20);
    assert(config.clean_session);
    assert(config.IsValid());

    const std::string timestamp = device::CurrentIso8601Timestamp();
    assert(logistics::contracts::mqtt::IsValidIso8601Timestamp(timestamp));
    assert(device::MakeMessageId(config.device_id, 42) == "PI-01-MSG-42");

    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

int main() {
    TestConfigLoading();
    return 0;
}
