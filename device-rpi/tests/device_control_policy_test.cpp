#include "logistics/device/device_control_policy.hpp"

#include <cassert>
#include <string>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/device_control_state.hpp"

namespace {

namespace device = logistics::device;
namespace mqtt = logistics::contracts::mqtt;

mqtt::MqttMessage Control(mqtt::ControlCommand command, std::string component = {},
                          std::string request_id = "REQ-CONTROL") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-CONTROL",
        .message_type = command == mqtt::ControlCommand::kEmergencyStop ? mqtt::MessageType::kEmergencyStop
                                                                        : mqtt::MessageType::kControlCommand,
        .source_id = "central-server",
        .timestamp = "2026-07-29T10:00:00+09:00",
        .data = command == mqtt::ControlCommand::kEmergencyStop ? mqtt::MessagePayload{ mqtt::EmergencyStopPayload{
                                                                      .request_id = request_id,
                                                                      .command = command,
                                                                      .target_device_id = "ALL",
                                                                  } }
                                                                : mqtt::MessagePayload{ mqtt::ControlCommandPayload{
                                                                      .request_id = std::move(request_id),
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

void TestDuplicateRecoveryPreservesOriginalOwnerUntilCompletion() {
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

    const auto recovery = control.HandleCommand(Control(mqtt::ControlCommand::kRecovery, {}, "REQ-RECOVERY-OWNER"),
                                                "MSG-RECOVERY", "2026-07-29T10:00:03+09:00");
    assert(recovery.has_value());
    assert(recovery->clear_work);
    assert(control.ConsumeResetRequest());

    const auto retry_before_ready =
        control.HandleCommand(Control(mqtt::ControlCommand::kRecovery, {}, "REQ-RECOVERY-DUPLICATE-BEFORE-READY"),
                              "MSG-RECOVERY-RETRY-1", "2026-07-29T10:00:04+09:00");
    assert(retry_before_ready.has_value());
    const auto* before_ready_response = mqtt::GetPayload<mqtt::CommandResponsePayload>(retry_before_ready->response);
    assert(before_ready_response != nullptr && before_ready_response->result == mqtt::CommandResult::kProcessing);
    assert(!retry_before_ready->clear_work);
    assert(!retry_before_ready->state_changed);
    assert(!control.ConsumeResetRequest());
    assert(control.State() == device::DeviceOperatingState::kRecovering);

    control.SetReady(true);
    const auto retry_after_ready =
        control.HandleCommand(Control(mqtt::ControlCommand::kRecovery, {}, "REQ-RECOVERY-DUPLICATE-AFTER-READY"),
                              "MSG-RECOVERY-RETRY-2", "2026-07-29T10:00:05+09:00");
    assert(retry_after_ready.has_value());
    const auto* after_ready_response = mqtt::GetPayload<mqtt::CommandResponsePayload>(retry_after_ready->response);
    assert(after_ready_response != nullptr && after_ready_response->result == mqtt::CommandResult::kProcessing);
    assert(!retry_after_ready->clear_work);
    assert(!retry_after_ready->state_changed);
    assert(!control.ConsumeResetRequest());
    assert(control.State() == device::DeviceOperatingState::kRecovering);

    const auto completed = control.CompleteRecovery("MSG-RECOVERY-COMPLETE", "2026-07-29T10:00:06+09:00");
    assert(completed.has_value());
    const auto* completed_response = mqtt::GetPayload<mqtt::CommandResponsePayload>(*completed);
    assert(completed_response != nullptr && completed_response->request_id == "REQ-RECOVERY-OWNER" &&
           completed_response->result == mqtt::CommandResult::kSuccess);
    assert(control.State() == device::DeviceOperatingState::kStopped);
    assert(!control.CompleteRecovery("MSG-RECOVERY-EXTRA", "2026-07-29T10:00:07+09:00").has_value());
}

}  // namespace

int main() {
    TestCommonActions();
    TestCommonTargets();
    TestRecoveryDefaultsToSafety();
    TestRequestsUseOneView();
    TestControlSpeedParsing();
    TestDuplicateRecoveryPreservesOriginalOwnerUntilCompletion();
    return 0;
}
