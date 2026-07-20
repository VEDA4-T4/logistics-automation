#include "logistics/central_server/device_manager.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
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

    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/register"), registration, "2026-07-15T08:30:01Z"));
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

    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/heartbeat"), heartbeat, "2026-07-15T08:30:02Z"));
    device = manager.FindDevice("PI-01");
    assert(device.has_value());
    assert(device->current_state == "RUNNING");
    assert(device->uptime == 42);
    assert(device->job_id == std::optional<std::string>("JOB-0001"));
    assert(device->last_message_timestamp == heartbeat.timestamp);
    assert(device->last_heartbeat_timestamp == heartbeat.timestamp);
    assert(device->last_seen_timestamp == "2026-07-15T08:30:02Z");
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

    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/heartbeat"), heartbeat));
    assert(manager.RegisteredDeviceCount() == 0);
    assert(manager.FindDevice("PI-01").has_value());
}

void TestRegisteredDevicesSurviveServerRestart() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() / ("logistics-device-registry-" + std::to_string(suffix) + ".json");

    {
        central_server::DeviceManager manager(path);
        const auto registration = MakeMessage("MSG-REGISTER-PERSIST", mqtt::MessageType::kDeviceRegister,
                                              mqtt::DeviceRegisterPayload{
                                                  .device_type = "camera-node",
                                                  .node_name = "camera-node-01",
                                                  .status = mqtt::ConnectionState::kOnline,
                                                  .ip_address = "192.168.0.21",
                                                  .uart_connected = true,
                                              });
        assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/register"), registration, "2026-07-15T08:30:03Z"));
        auto heartbeat = MakeMessage("MSG-HEARTBEAT-PERSIST", mqtt::MessageType::kHeartbeat,
                                     mqtt::HeartbeatPayload{
                                         .status = mqtt::ConnectionState::kOnline,
                                         .current_state = "IDLE",
                                         .uptime = 10,
                                         .job_id = std::nullopt,
                                         .error_code = std::nullopt,
                                     });
        heartbeat.timestamp = "2026-07-15T08:34:00Z";
        assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/heartbeat"), heartbeat, "2026-07-15T08:34:00Z"));
        auto last_will = MakeMessage("MSG-WILL-PERSIST", mqtt::MessageType::kDeviceStatus,
                                     mqtt::DeviceStatusPayload{
                                         .status = mqtt::ConnectionState::kOffline,
                                         .current_state = "DISCONNECTED",
                                         .job_id = std::nullopt,
                                         .error_code = std::string("ERR-MQTT-DISCONNECTED"),
                                     });
        last_will.timestamp = "2026-07-15T08:00:00Z";
        assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/status"), last_will, "2026-07-15T08:35:00Z"));
        assert(std::filesystem::exists(path));
    }

    {
        central_server::DeviceManager restored(path);
        assert(restored.RegisteredDeviceCount() == 1);
        const auto device = restored.FindDevice("PI-01");
        assert(device.has_value());
        assert(device->registered);
        assert(device->device_type == "camera-node");
        assert(device->connection_state == mqtt::ConnectionState::kOffline);
        assert(device->current_state == "DISCONNECTED");
        assert(device->last_message_timestamp == "2026-07-15T08:00:00Z");
        assert(device->last_heartbeat_timestamp == "2026-07-15T08:34:00Z");
        assert(device->last_seen_timestamp == "2026-07-15T08:35:00Z");
        assert(device->disconnected_at == std::optional<std::string>("2026-07-15T08:35:00Z"));
    }

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestMalformedRegistryIsRejected() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path =
        std::filesystem::temp_directory_path() / ("logistics-invalid-registry-" + std::to_string(suffix) + ".json");
    {
        std::ofstream output(path);
        assert(output);
        output << '{';
    }

    bool rejected = false;
    try {
        static_cast<void>(central_server::DeviceManager(path));
    } catch (const central_server::DeviceRegistryError&) {
        rejected = true;
    }
    assert(rejected);

    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

int main() {
    TestDeviceRegistrationAndHeartbeat();
    TestHeartbeatDoesNotRegisterUnknownDevice();
    TestRegisteredDevicesSurviveServerRestart();
    TestMalformedRegistryIsRejected();
    return 0;
}
