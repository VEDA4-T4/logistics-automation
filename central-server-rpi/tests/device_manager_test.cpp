#include "logistics/central_server/device_manager.hpp"

#include <cassert>
#include <optional>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

[[nodiscard]] mqtt::MqttMessage MakeMessage(std::string message_id, mqtt::MessageType message_type,
                                            mqtt::MessagePayload payload) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = message_type,
        .source_id = "PI-01",
        .timestamp = "2026-07-15T17:30:00+09:00",
        .data = std::move(payload),
    };
}

void TestDeviceRegistrationAndHeartbeat() {
    central_server::DeviceManager manager;

    const auto registration = MakeMessage("MSG-REGISTER-01", mqtt::MessageType::kDeviceRegister,
                                          mqtt::DeviceRegisterPayload{
                                              .device_type = "camera-node",
                                              .node_name = "camera-node-01",
                                              .status = mqtt::ConnectionState::kOnline,
                                              .ip_address = "192.168.0.21",
                                              .uart_connected = true,
                                          });

    manager.HandleMessage(mqtt::ParseTopic("device/PI-01/register"), registration);
    assert(manager.RegisteredDeviceCount() == 1);

    auto device = manager.FindDevice("PI-01");
    assert(device.has_value());
    assert(device->registered);
    assert(device->device_type == "camera-node");
    assert(device->uart_connected);

    const auto heartbeat = MakeMessage("MSG-HEARTBEAT-01", mqtt::MessageType::kHeartbeat,
                                       mqtt::HeartbeatPayload{
                                           .status = mqtt::ConnectionState::kOnline,
                                           .current_state = "RUNNING",
                                           .uptime = 42,
                                           .job_id = std::string("JOB-0001"),
                                           .error_code = std::nullopt,
                                       });

    manager.HandleMessage(mqtt::ParseTopic("device/PI-01/heartbeat"), heartbeat);
    device = manager.FindDevice("PI-01");
    assert(device.has_value());
    assert(device->current_state == "RUNNING");
    assert(device->uptime == 42);
    assert(device->job_id == std::optional<std::string>("JOB-0001"));
    assert(device->last_message_timestamp == heartbeat.timestamp);
}

void TestHeartbeatDoesNotRegisterUnknownDevice() {
    central_server::DeviceManager manager;
    const auto heartbeat = MakeMessage("MSG-HEARTBEAT-02", mqtt::MessageType::kHeartbeat,
                                       mqtt::HeartbeatPayload{
                                           .status = mqtt::ConnectionState::kOnline,
                                           .current_state = "IDLE",
                                           .uptime = 1,
                                           .job_id = std::nullopt,
                                           .error_code = std::nullopt,
                                       });

    manager.HandleMessage(mqtt::ParseTopic("device/PI-01/heartbeat"), heartbeat);
    assert(manager.RegisteredDeviceCount() == 0);
    assert(manager.FindDevice("PI-01").has_value());
}

}  // namespace

int main() {
    TestDeviceRegistrationAndHeartbeat();
    TestHeartbeatDoesNotRegisterUnknownDevice();
    return 0;
}
