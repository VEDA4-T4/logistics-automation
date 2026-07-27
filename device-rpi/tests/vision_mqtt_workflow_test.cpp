#include "vision_mqtt_workflow.hpp"

#include <cassert>
#include <optional>
#include <string>

#include "logistics/contracts/mqtt_validation.hpp"
#include "logistics/device/device_control_state.hpp"

namespace {

namespace mqtt = logistics::contracts::mqtt;
namespace device = logistics::device;
namespace vision = logistics::vision;

constexpr std::string_view kWorkId = "22a194c3-3e3c-410c-a329-7e8c4ebcac83";

vision::VisionObservation Observation(std::optional<std::string> barcode = std::nullopt) {
    return {
        .image_name = "capture-01.jpg",
        .box_x = 100,
        .box_y = 50,
        .box_width = 200,
        .box_height = 100,
        .frame_width = 640,
        .frame_height = 480,
        .barcode = std::move(barcode),
    };
}

mqtt::MqttMessage WorkCreated() {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "WORK-ASSIGN-01",
        .message_type = mqtt::MessageType::kWorkCreated,
        .source_id = "central-server",
        .timestamp = "2026-07-21T11:00:01Z",
        .data = mqtt::WorkCreatedPayload{ .work_id = std::string(kWorkId) },
    };
}

mqtt::MqttMessage ControlCommand(const mqtt::ControlCommand command, std::string request_id = "REQ-VISION-01",
                                 std::string target_device_id = "PI-VISION-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-CONTROL-01",
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "central-server",
        .timestamp = "2026-07-21T11:00:01Z",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = std::move(request_id),
                .command = command,
                .target_device_id = std::move(target_device_id),
                .component_id = {},
                .params = mqtt::Json::object(),
            },
    };
}

mqtt::MqttMessage EmergencyStop() {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-ESTOP-01",
        .message_type = mqtt::MessageType::kEmergencyStop,
        .source_id = "central-server",
        .timestamp = "2026-07-21T11:00:01Z",
        .data =
            mqtt::EmergencyStopPayload{
                .request_id = "REQ-ESTOP-01",
                .command = mqtt::ControlCommand::kEmergencyStop,
                .target_device_id = "ALL",
            },
    };
}

const mqtt::CommandResponsePayload& ResponsePayload(const device::DeviceControlDecision& decision) {
    const auto* response = mqtt::GetPayload<mqtt::CommandResponsePayload>(decision.response);
    assert(response != nullptr);
    return *response;
}

device::DeviceControlDecision Handle(device::DeviceControlState& control, const mqtt::MqttMessage& command,
                                     const int sequence) {
    auto decision = control.HandleCommand(command, "MSG-RESPONSE-" + std::to_string(sequence), "2026-07-21T11:00:02Z");
    assert(decision.has_value());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceResponseTopic("PI-VISION-01"), decision->response).IsSuccess());
    return std::move(*decision);
}

void TestDetectionAssignmentAndResultMessages() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 2, 2);
    assert(!workflow.Observe(Observation(), "MSG-BOX-00", "2026-07-21T11:00:00Z").has_value());
    const auto box = workflow.Observe(Observation(), "MSG-BOX-01", "2026-07-21T11:00:00Z");
    assert(box.has_value());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), *box).IsSuccess());

    assert(!workflow.HasPendingBarcode());
    assert(workflow.AssignWork(WorkCreated()));
    assert(!workflow.HasPendingBarcode());
    assert(!workflow.Observe(Observation(std::string("8801234567893")), "IGNORED", "2026-07-21T11:00:01Z").has_value());
    assert(workflow.HasPendingBarcode());
    const auto assigned = workflow.TakeAssignedWork();
    assert(assigned.has_value());
    assert(assigned->observation.barcode == "8801234567893");

    const auto position =
        vision::MakePositionDetectedMessage("PI-VISION-01", *assigned, "MSG-POSITION-01", "2026-07-21T11:00:02Z");
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *assigned, "MSG-BARCODE-01", "2026-07-21T11:00:02Z");
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), position).IsSuccess());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), barcode).IsSuccess());
    const auto image = vision::MakeProductImageMessage(
        "PI-VISION-01", kWorkId, "42f8e6f1-1277-4748-9e5e-c41c7bf605f7",
        "/uploads/images/42f8e6f1-1277-4748-9e5e-c41c7bf605f7.jpg",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "MSG-IMAGE-01", "2026-07-21T11:00:02Z");
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), image).IsSuccess());

    workflow.CompleteWork();
    assert(!workflow.Observe(std::nullopt, "IGNORED", "2026-07-21T11:00:03Z").has_value());
    assert(!workflow.Observe(std::nullopt, "IGNORED", "2026-07-21T11:00:04Z").has_value());
    assert(!workflow.Observe(Observation(), "MSG-BOX-02", "2026-07-21T11:00:05Z").has_value());
    assert(workflow.Observe(Observation(), "MSG-BOX-03", "2026-07-21T11:00:06Z").has_value());
}

void TestMissingBarcodeProducesFailedResult() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 1, 1, 2);
    assert(workflow.Observe(Observation(), "MSG-BOX-01", "2026-07-21T11:00:00Z").has_value());
    assert(!workflow.HasPendingBarcode());
    assert(workflow.AssignWork(WorkCreated()));
    assert(!workflow.TakeAssignedWork().has_value());
    assert(!workflow.Observe(Observation(), "IGNORED-01", "2026-07-21T11:00:01Z").has_value());
    assert(!workflow.TakeAssignedWork().has_value());
    assert(!workflow.Observe(Observation(), "IGNORED-02", "2026-07-21T11:00:02Z").has_value());
    const auto work = workflow.TakeAssignedWork();
    assert(work.has_value());
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *work, "MSG-BARCODE-FAIL", "2026-07-21T11:00:02Z");
    const auto* payload = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(barcode);
    assert(payload != nullptr);
    assert(payload->recognition_status == "FAILED");
    assert(payload->barcode.empty());
    assert(payload->message.has_value());
}

void TestVisionControlLifecycle() {
    device::DeviceControlState control({
        .device_id = "PI-VISION-01",
        .component_name = "vision",
        .not_ready_error_code = "ERR-CAMERA-UNAVAILABLE",
    });
    assert(control.State() == device::DeviceOperatingState::kStopped);
    assert(!control.IsOperational());

    auto decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 1);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kRejected);
    assert(ResponsePayload(decision).error_code == "ERR-CAMERA-UNAVAILABLE");

    control.SetReady(true);
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 2);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(control.State() == device::DeviceOperatingState::kRunning);
    assert(control.IsOperational());

    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStop), 3);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(decision.clear_work);
    assert(control.State() == device::DeviceOperatingState::kStopped);

    static_cast<void>(Handle(control, ControlCommand(mqtt::ControlCommand::kRestart), 4));
    decision = Handle(control, EmergencyStop(), 5);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(decision.clear_work);
    assert(control.State() == device::DeviceOperatingState::kEmergencyStop);

    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 6);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kRejected);

    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kRecovery), 7);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kProcessing);
    assert(control.State() == device::DeviceOperatingState::kRecovering);
    assert(control.ConsumeResetRequest());
    assert(!control.ConsumeResetRequest());

    control.SetReady(false);
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kInitialize), 8);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kRejected);
    assert(ResponsePayload(decision).error_code == "ERR-CAMERA-UNAVAILABLE");

    control.SetReady(true);
    const auto recovery_completed = control.CompleteRecovery("MSG-RECOVERY-COMPLETE", "2026-07-21T11:00:03Z");
    assert(recovery_completed.has_value());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceResponseTopic("PI-VISION-01"), *recovery_completed).IsSuccess());
    const auto* recovery_response = mqtt::GetPayload<mqtt::CommandResponsePayload>(*recovery_completed);
    assert(recovery_response != nullptr);
    assert(recovery_response->request_id == "REQ-VISION-01");
    assert(recovery_response->command == mqtt::ControlCommand::kRecovery);
    assert(recovery_response->result == mqtt::CommandResult::kSuccess);
    assert(!control.CompleteRecovery("IGNORED", "2026-07-21T11:00:04Z").has_value());
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kInitialize), 9);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(control.State() == device::DeviceOperatingState::kStopped);

    static_cast<void>(Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 10));
    control.SetReady(false);
    assert(control.State() == device::DeviceOperatingState::kError);
    assert(!control.IsOperational());
}

void TestStopClearsPendingVisionWork() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 1, 1);
    device::DeviceControlState control({
        .device_id = "PI-VISION-01",
        .component_name = "vision",
        .not_ready_error_code = "ERR-CAMERA-UNAVAILABLE",
    });
    control.SetReady(true);
    static_cast<void>(Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 1));

    assert(workflow.Observe(Observation("8801234567893"), "MSG-BOX-01", "2026-07-21T11:00:00Z").has_value());
    assert(workflow.AssignWork(WorkCreated()));
    const auto decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStop), 2);
    assert(decision.clear_work);
    workflow.Reset();
    assert(!workflow.TakeAssignedWork().has_value());
    assert(!control.IsOperational());
}

}  // namespace

int main() {
    TestDetectionAssignmentAndResultMessages();
    TestMissingBarcodeProducesFailedResult();
    TestVisionControlLifecycle();
    TestStopClearsPendingVisionWork();
    return 0;
}
