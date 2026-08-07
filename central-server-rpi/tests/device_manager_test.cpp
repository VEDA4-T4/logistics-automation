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

[[nodiscard]] mqtt::MqttMessage MakePositionStatus(std::string message_id = "MSG-POSITION-01") {
    return MakeMessage(
        std::move(message_id), mqtt::MessageType::kDeviceStatus,
        mqtt::DeviceStatusPayload{
            .status = mqtt::ConnectionState::kOnline,
            .current_state = "FOLLOWING_LINE",
            .job_id = std::string("JOB-0001"),
            .error_code = std::nullopt,
            .departure_position = mqtt::LineTracerPositionPayload{ .area = "DEPARTURE", .location = "A" },
            .target_position = mqtt::LineTracerPositionPayload{ .area = "DESTINATION", .location = "C" },
            .confirmed_position = mqtt::LineTracerPositionPayload{ .area = "DEPARTURE", .location = "A" },
            .movement_state = std::string("MOVING"),
        });
}

void AssertPositionRetained(const central_server::DeviceSnapshot& device) {
    assert(device.departure_position.has_value());
    assert(device.departure_position->area == "DEPARTURE");
    assert(device.departure_position->location == "A");
    assert(device.target_position.has_value());
    assert(device.target_position->area == "DESTINATION");
    assert(device.target_position->location == "C");
    assert(device.confirmed_position.has_value());
    assert(device.confirmed_position->area == "DEPARTURE");
    assert(device.confirmed_position->location == "A");
    assert(device.movement_state == std::optional<std::string>("MOVING"));
    assert(!device.position_reset);
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

void TestHeartbeatTimeoutTransitionsAndRecovery() {
    central_server::DeviceManager::Clock::time_point now{};
    central_server::DeviceManager manager({}, [&now] { return now; });
    const auto registration = MakeMessage("MSG-REGISTER-TIMEOUT", mqtt::MessageType::kDeviceRegister,
                                          mqtt::DeviceRegisterPayload{
                                              .device_type = "camera-node",
                                              .node_name = "camera-node-01",
                                              .status = mqtt::ConnectionState::kOnline,
                                              .ip_address = "192.168.0.21",
                                              .uart_connected = true,
                                          });
    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/register"), registration, "2026-07-15T08:30:00Z"));

    now += std::chrono::seconds(9);
    assert(manager.CheckHeartbeatTimeouts("2026-07-15T08:30:09Z").empty());

    now += std::chrono::seconds(1);
    const auto delayed = manager.CheckHeartbeatTimeouts("2026-07-15T08:30:10Z");
    assert(delayed.size() == 1);
    assert(delayed[0].connection_state == mqtt::ConnectionState::kDelayed);
    assert(!delayed[0].disconnected_at.has_value());
    assert(manager.CheckHeartbeatTimeouts("2026-07-15T08:30:10Z").empty());

    now += std::chrono::seconds(5);
    const auto offline = manager.CheckHeartbeatTimeouts("2026-07-15T08:30:15Z");
    assert(offline.size() == 1);
    assert(offline[0].connection_state == mqtt::ConnectionState::kOffline);
    assert(offline[0].disconnected_at == std::optional<std::string>("2026-07-15T08:30:15Z"));
    assert(offline[0].error_code == std::optional<std::string>("ERR-HEARTBEAT-TIMEOUT"));

    auto heartbeat = MakeMessage("MSG-HEARTBEAT-RECOVERY", mqtt::MessageType::kHeartbeat,
                                 mqtt::HeartbeatPayload{
                                     .status = mqtt::ConnectionState::kOnline,
                                     .current_state = "IDLE",
                                     .uptime = 60,
                                     .job_id = std::nullopt,
                                     .error_code = std::nullopt,
                                 });
    heartbeat.timestamp = "2026-07-15T08:30:16Z";
    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/heartbeat"), heartbeat, heartbeat.timestamp));
    const auto recovered = manager.FindDevice("PI-01");
    assert(recovered.has_value());
    assert(recovered->connection_state == mqtt::ConnectionState::kOnline);
    assert(!recovered->disconnected_at.has_value());

    now += std::chrono::seconds(10);
    const auto delayed_again = manager.CheckHeartbeatTimeouts("2026-07-15T08:30:26Z");
    assert(delayed_again.size() == 1);
    assert(delayed_again[0].connection_state == mqtt::ConnectionState::kDelayed);
}

void TestHeartbeatAndLegacyStatusPreservePositionUntilExplicitReset() {
    central_server::DeviceManager manager;
    const auto registration = MakeMessage("MSG-REGISTER-POSITION", mqtt::MessageType::kDeviceRegister,
                                          mqtt::DeviceRegisterPayload{
                                              .device_type = "line-tracer",
                                              .node_name = "line-tracer-node-01",
                                              .status = mqtt::ConnectionState::kOnline,
                                              .ip_address = "192.168.0.31",
                                              .uart_connected = true,
                                          });
    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/register"), registration));
    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/status"), MakePositionStatus()));

    const auto heartbeat = MakeMessage("MSG-HEARTBEAT-POSITION", mqtt::MessageType::kHeartbeat,
                                       mqtt::HeartbeatPayload{
                                           .status = mqtt::ConnectionState::kOnline,
                                           .current_state = "FOLLOWING_LINE",
                                           .uptime = 100,
                                           .job_id = std::string("JOB-0001"),
                                           .error_code = std::nullopt,
                                       });
    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/heartbeat"), heartbeat));

    const auto legacy_status = MakeMessage("MSG-LEGACY-STATUS", mqtt::MessageType::kDeviceStatus,
                                           mqtt::DeviceStatusPayload{
                                               .status = mqtt::ConnectionState::kOffline,
                                               .current_state = "DISCONNECTED",
                                               .job_id = std::nullopt,
                                               .error_code = std::string("ERR-MQTT-DISCONNECTED"),
                                           });
    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/status"), legacy_status));
    auto device = manager.FindDevice("PI-01");
    assert(device.has_value());
    AssertPositionRetained(*device);

    const auto reset = MakeMessage("MSG-POSITION-RESET", mqtt::MessageType::kDeviceStatus,
                                   mqtt::DeviceStatusPayload{
                                       .status = mqtt::ConnectionState::kOnline,
                                       .current_state = "POSITION_UNKNOWN",
                                       .job_id = std::nullopt,
                                       .error_code = std::nullopt,
                                       .position_reset = true,
                                   });
    assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/status"), reset));
    device = manager.FindDevice("PI-01");
    assert(device.has_value());
    assert(!device->departure_position.has_value());
    assert(!device->target_position.has_value());
    assert(!device->confirmed_position.has_value());
    assert(!device->movement_state.has_value());
    assert(device->position_reset);
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
        assert(manager.HandleMessage(mqtt::ParseTopic("device/PI-01/status"), MakePositionStatus(),
                                     "2026-07-15T08:33:00Z"));
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
        AssertPositionRetained(*device);
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
    TestHeartbeatTimeoutTransitionsAndRecovery();
    TestHeartbeatAndLegacyStatusPreservePositionUntilExplicitReset();
    TestRegisteredDevicesSurviveServerRestart();
    TestMalformedRegistryIsRejected();
    return 0;
}
