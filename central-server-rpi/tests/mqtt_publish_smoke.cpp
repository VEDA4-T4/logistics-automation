#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "logistics/central_server/mqtt_client.hpp"
#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/central_server/mqtt_transport.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

[[nodiscard]] std::string CurrentIso8601Timestamp() {
    const std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
    {
        static std::mutex time_mutex;
        std::lock_guard lock(time_mutex);
        const std::tm* converted = std::gmtime(&current_time);
        if (converted == nullptr) {
            return {};
        }
        utc_time = *converted;
    }
    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] std::string SmokeIdSuffix() {
    std::ostringstream output;
    output << std::hex << std::chrono::system_clock::now().time_since_epoch().count();
    return output.str();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3 || argv[1] == nullptr || argv[2] == nullptr) {
        std::cerr << "usage: central_server_mqtt_smoke <server.ini> <device-id>\n";
        return 2;
    }

    const std::string device_id = argv[2];
    if (!mqtt::IsValidTopicLevel(device_id)) {
        std::cerr << "invalid device ID\n";
        return 2;
    }

    central_server::MqttConfig config;
    try {
        config = central_server::LoadMqttConfig(std::filesystem::path(argv[1]));
    } catch (const central_server::ConfigError& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }

    const std::string suffix = SmokeIdSuffix();
    const std::string source_id = config.client_id;
    config.client_id += "-smoke-" + suffix;
    central_server::MqttClient client(std::move(config), central_server::CreateMosquittoTransport());
    if (!client.Start()) {
        return 3;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!client.IsConnected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!client.IsConnected()) {
        std::cerr << "smoke client did not connect within 5 seconds\n";
        client.Stop();
        return 4;
    }

    const mqtt::MqttMessage command{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "SMOKE-MSG-" + suffix,
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = source_id,
        .timestamp = CurrentIso8601Timestamp(),
        .data =
            mqtt::ControlCommandPayload{
                .request_id = "SMOKE-REQ-" + suffix,
                .command = mqtt::ControlCommand::kStatusRequest,
                .target_device_id = device_id,
                .component_id = {},
                .params = mqtt::Json::object(),
            },
    };

    const std::string topic = mqtt::DeviceCommandTopic(device_id);
    if (!client.PublishMessage(topic, command, mqtt::Qos::kAtLeastOnce, false)) {
        client.Stop();
        return 5;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    client.Stop();
    std::cout << "smoke publish succeeded: topic=" << topic << "; messageId=" << command.message_id << '\n';
    return 0;
}
