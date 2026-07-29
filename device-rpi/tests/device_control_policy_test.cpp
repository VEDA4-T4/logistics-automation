#include "logistics/device/device_control_policy.hpp"

#include <cassert>
#include <string>

#include "logistics/contracts/mqtt_codec.hpp"

namespace {

namespace device = logistics::device;
namespace mqtt = logistics::contracts::mqtt;

mqtt::MqttMessage Control(mqtt::ControlCommand command, std::string component = {}) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-CONTROL",
        .message_type = command == mqtt::ControlCommand::kEmergencyStop ? mqtt::MessageType::kEmergencyStop
                                                                        : mqtt::MessageType::kControlCommand,
        .source_id = "central-server",
        .timestamp = "2026-07-29T10:00:00+09:00",
        .data = command == mqtt::ControlCommand::kEmergencyStop ? mqtt::MessagePayload{ mqtt::EmergencyStopPayload{
                                                                      .request_id = "REQ-CONTROL",
                                                                      .command = command,
                                                                      .target_device_id = "ALL",
                                                                  } }
                                                                : mqtt::MessagePayload{ mqtt::ControlCommandPayload{
                                                                      .request_id = "REQ-CONTROL",
                                                                      .command = command,
                                                                      .target_device_id = "PI-01",
                                                                      .component_id = std::move(component),
                                                                      .params = mqtt::Json::object(),
                                                                  } },
    };
}

void TestCommonActions() {
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kStart) == device::DeviceControlAction::kStart);
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kRestart) == device::DeviceControlAction::kStart);
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kStop) == device::DeviceControlAction::kStop);
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kStatusRequest) ==
           device::DeviceControlAction::kStatusRequest);
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kEmergencyStop) ==
           device::DeviceControlAction::kEmergencyStop);
}

void TestRecoveryDefaultsToSafety() {
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kRecovery) ==
           device::DeviceControlAction::kSafetyRecovery);
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kRecovery, "safety") ==
           device::DeviceControlAction::kSafetyRecovery);
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kRecovery, "SAFE-TY") ==
           device::DeviceControlAction::kSafetyRecovery);
    assert(device::ResolveDeviceControlAction(mqtt::ControlCommand::kRecovery, "GATE") ==
           device::DeviceControlAction::kComponentRecovery);
}

void TestRequestsUseOneView() {
    const auto restart = device::ReadDeviceControlRequest(Control(mqtt::ControlCommand::kRestart));
    assert(restart.has_value());
    assert(restart->command == mqtt::ControlCommand::kRestart);
    assert(restart->request_id == "REQ-CONTROL");
    assert(restart->target_device_id == "PI-01");
    assert(device::ResolveDeviceControlAction(restart->command, restart->component_id) ==
           device::DeviceControlAction::kStart);

    const auto emergency = device::ReadDeviceControlRequest(Control(mqtt::ControlCommand::kEmergencyStop));
    assert(emergency.has_value());
    assert(emergency->command == mqtt::ControlCommand::kEmergencyStop);
    assert(emergency->target_device_id == "ALL");
}

}  // namespace

int main() {
    TestCommonActions();
    TestRecoveryDefaultsToSafety();
    TestRequestsUseOneView();
    return 0;
}
