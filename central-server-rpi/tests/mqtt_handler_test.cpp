#include "logistics/central_server/mqtt_handler.hpp"

#include <cassert>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logistics/central_server/device_manager.hpp"
#include "logistics/contracts/mqtt_codec.hpp"

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

struct LogEntry final {
    central_server::MqttHandlerLogLevel level;
    std::string message;
};

[[nodiscard]] mqtt::MqttMessage MakeRegistration(std::string source_id = "PI-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-REGISTER-01",
        .message_type = mqtt::MessageType::kDeviceRegister,
        .source_id = std::move(source_id),
        .timestamp = "2026-07-16T01:00:00Z",
        .data =
            mqtt::DeviceRegisterPayload{
                .device_type = "vision",
                .node_name = "vision-node-01",
                .status = mqtt::ConnectionState::kOnline,
                .ip_address = "192.168.0.21",
                .uart_connected = false,
            },
    };
}

[[nodiscard]] std::string Encode(const mqtt::MqttMessage& message) {
    const auto encoded = mqtt::SerializeMessage(message);
    assert(encoded.IsSuccess());
    return encoded.payload;
}

void TestRegistrationIsDecodedValidatedAndRouted() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(
        device_manager, [&logs](central_server::MqttHandlerLogLevel level, std::string_view message) {
            logs.push_back({ .level = level, .message = std::string(message) });
        });

    assert(handler.Handle("device/PI-01/register", Encode(MakeRegistration()), "2026-07-16T01:00:01Z"));
    assert(device_manager.RegisteredDeviceCount() == 1);

    const auto device = device_manager.FindDevice("PI-01");
    assert(device.has_value());
    assert(device->registered);
    assert(device->device_type == "vision");
    assert(device->last_seen_timestamp == "2026-07-16T01:00:01Z");
    assert(logs.size() == 2);
    assert(logs.front().level == central_server::MqttHandlerLogLevel::kInfo);
    assert(logs.front().message == "MQTT message received: device/PI-01/register");
    assert(logs.back().message.find("registered devices=1") != std::string::npos);
}

void TestMalformedJsonIsRejected() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(
        device_manager, [&logs](central_server::MqttHandlerLogLevel level, std::string_view message) {
            logs.push_back({ .level = level, .message = std::string(message) });
        });

    assert(!handler.Handle("device/PI-01/register", "{"));
    assert(device_manager.RegisteredDeviceCount() == 0);
    assert(logs.size() == 1);
    assert(logs.front().level == central_server::MqttHandlerLogLevel::kError);
    assert(logs.front().message.find("invalid MQTT JSON") != std::string::npos);
}

void TestTopicMessageMismatchIsRejected() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(
        device_manager, [&logs](central_server::MqttHandlerLogLevel level, std::string_view message) {
            logs.push_back({ .level = level, .message = std::string(message) });
        });

    assert(!handler.Handle("device/PI-02/register", Encode(MakeRegistration("PI-01"))));
    assert(device_manager.RegisteredDeviceCount() == 0);
    assert(logs.size() == 1);
    assert(logs.front().level == central_server::MqttHandlerLogLevel::kError);
    assert(logs.front().message.find("SOURCE_ID_MISMATCH") != std::string::npos);
}

}  // namespace

int main() {
    TestRegistrationIsDecodedValidatedAndRouted();
    TestMalformedJsonIsRejected();
    TestTopicMessageMismatchIsRejected();
    return 0;
}
