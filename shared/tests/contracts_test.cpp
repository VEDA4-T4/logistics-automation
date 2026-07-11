#include <cassert>
#include <chrono>
#include <stdexcept>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/process.hpp"

int main() {
    using logistics::contracts::DeviceRole;
    using logistics::contracts::ProcessState;

    assert(logistics::contracts::ToString(DeviceRole::kCamera) == "camera");
    assert(logistics::contracts::ToString(ProcessState::kEmergencyStop) == "ESTOP");

    namespace mqtt = logistics::contracts::mqtt;
    assert(mqtt::QtRequestTopic("QT-01") == "server/request/QT-01");
    assert(mqtt::QtResponseTopic("QT-01") == "qt/QT-01/response");
    assert(mqtt::DeviceCommandTopic("PI-LT-01") == "device/PI-LT-01/command");
    assert(mqtt::DeviceHeartbeatTopic("PI-01") == "device/PI-01/heartbeat");

    const auto qt_request = mqtt::ParseTopic("server/request/QT-01");
    assert(qt_request.kind == mqtt::TopicKind::kQtRequest);
    assert(qt_request.endpoint_id == "QT-01");

    const auto qt_status = mqtt::ParseTopic("qt/QT-01/status");
    assert(qt_status.kind == mqtt::TopicKind::kQtStatus);
    assert(qt_status.endpoint_id == "QT-01");

    const auto device_event = mqtt::ParseTopic("device/PI-01/event");
    assert(device_event.kind == mqtt::TopicKind::kDeviceEvent);
    assert(device_event.endpoint_id == "PI-01");
    assert(!mqtt::ParseTopic("device/+/event").IsValid());
    assert(!mqtt::ParseTopic("device/PI-01/unknown").IsValid());
    assert(mqtt::ParseTopic(mqtt::kSystemBroadcastCommandTopic).kind == mqtt::TopicKind::kSystemBroadcastCommand);
    assert(mqtt::ParseTopic(mqtt::kServerHeartbeatTopic).kind == mqtt::TopicKind::kServerHeartbeat);

    bool invalid_id_rejected = false;
    try {
        static_cast<void>(mqtt::DeviceStatusTopic("PI/01"));
    } catch (const std::invalid_argument&) {
        invalid_id_rejected = true;
    }
    assert(invalid_id_rejected);

    assert(mqtt::MessageTypeFromString("CONTROL_COMMAND") == mqtt::MessageType::kControlCommand);
    assert(mqtt::ControlCommandFromString("EMERGENCY_STOP") == mqtt::ControlCommand::kEmergencyStop);
    assert(mqtt::CommandResultFromString("RECEIVED") == mqtt::CommandResult::kReceived);
    assert(mqtt::ToString(mqtt::CommandResult::kDuplicated) == "DUPLICATED");
    assert(mqtt::IsTerminal(mqtt::CommandResult::kSuccess));
    assert(!mqtt::IsTerminal(mqtt::CommandResult::kProcessing));

    const auto heartbeat_policy = mqtt::PolicyFor(mqtt::MessageType::kHeartbeat);
    assert(heartbeat_policy.minimum_qos == mqtt::Qos::kAtMostOnce);
    assert(heartbeat_policy.retain == mqtt::RetainPolicy::kNever);

    const auto status_policy = mqtt::PolicyFor(mqtt::MessageType::kDeviceStatus);
    assert(status_policy.maximum_qos == mqtt::Qos::kAtLeastOnce);
    assert(status_policy.retain == mqtt::RetainPolicy::kLatestStateAllowed);

    constexpr mqtt::EnvelopeView valid_envelope{ mqtt::MessageType::kHeartbeat, "PI-01", "2026-08-20T14:31:30", "{}" };
    static_assert(valid_envelope.IsValid());
    constexpr mqtt::EnvelopeView invalid_envelope{ mqtt::MessageType::kUnknown, "PI-01", "2026-08-20T14:31:30", "{}" };
    static_assert(!invalid_envelope.IsValid());

    constexpr mqtt::CommandRequestView command_request{ "REQ-0001", mqtt::ControlCommand::kStart, "PI-01" };
    static_assert(command_request.IsValid());
    constexpr mqtt::CommandResponseView command_response{ "REQ-0001", mqtt::ControlCommand::kStart,
                                                         mqtt::CommandResult::kReceived };
    static_assert(command_response.IsValid());

    static_assert(mqtt::kHeartbeatInterval.count() == 5);
    static_assert(mqtt::kHeartbeatDelayedAfter.count() == 10);
    static_assert(mqtt::kHeartbeatOfflineAfter.count() == 15);
    static_assert(mqtt::kMqttMaximumRetries == 3);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 9 }) == mqtt::ConnectionState::kOnline);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 10 }) == mqtt::ConnectionState::kDelayed);
    static_assert(mqtt::ConnectionStateForHeartbeatAge(std::chrono::seconds{ 15 }) == mqtt::ConnectionState::kOffline);
    return 0;
}
