#include "logistics/device/device_control_policy.hpp"

#include <cassert>
#include <string>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/device_control_state.hpp"

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

void TestCommonTargets() {
    assert(device::IsControlTargetForDevice("PI-01", "PI-01"));
    assert(device::IsControlTargetForDevice("ALL", "PI-01"));
    assert(device::IsControlTargetForDevice("SYSTEM", "PI-01"));
    assert(!device::IsControlTargetForDevice("PI-02", "PI-01"));
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

void TestControlSpeedParsing() {
    bool invalid = true;
    assert(!device::ReadControlSpeed(mqtt::Json::object(), 100U, invalid).has_value() && !invalid);
    assert(device::ReadControlSpeed(mqtt::Json{ { "speed", 42 } }, 100U, invalid) == 42U && !invalid);
    assert(!device::ReadControlSpeed(mqtt::Json{ { "speed", 0 } }, 100U, invalid).has_value() && invalid);
    assert(!device::ReadControlSpeed(mqtt::Json{ { "speed", 101 } }, 100U, invalid).has_value() && invalid);
    assert(!device::ReadControlSpeed(mqtt::Json{ { "speed", "fast" } }, 100U, invalid).has_value() && invalid);
}

void TestSafetyRecoveryAlwaysClearsWork() {
    device::DeviceControlState control({
        .device_id = "PI-01",
        .component_name = "test",
    });
    control.SetReady(true);
    assert(control.HandleCommand(Control(mqtt::ControlCommand::kStart), "MSG-START", "2026-07-29T10:00:01+09:00")
               .has_value());
    assert(
        control.HandleCommand(Control(mqtt::ControlCommand::kEmergencyStop), "MSG-ESTOP", "2026-07-29T10:00:02+09:00")
            .has_value());

    const auto recovery =
        control.HandleCommand(Control(mqtt::ControlCommand::kRecovery), "MSG-RECOVERY", "2026-07-29T10:00:03+09:00");
    assert(recovery.has_value());
    assert(recovery->clear_work);
    assert(control.ConsumeResetRequest());

    const auto retry = control.HandleCommand(Control(mqtt::ControlCommand::kRecovery), "MSG-RECOVERY-RETRY",
                                             "2026-07-29T10:00:04+09:00");
    assert(retry.has_value());
    assert(retry->clear_work);
    assert(control.ConsumeResetRequest());
    assert(!control.ConsumeResetRequest());
}

}  // namespace

int main() {
    TestCommonActions();
    TestCommonTargets();
    TestRecoveryDefaultsToSafety();
    TestRequestsUseOneView();
    TestControlSpeedParsing();
    TestSafetyRecoveryAlwaysClearsWork();
    return 0;
}
