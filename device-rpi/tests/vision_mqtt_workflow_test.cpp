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

vision::VisionObservation Observation(std::optional<std::string> barcode = std::nullopt,
                                      const bool barcode_region_detected = false) {
    return {
        .image_name = "capture-01.jpg",
        .box_x = 100,
        .box_y = 50,
        .box_width = 200,
        .box_height = 100,
        .frame_width = 640,
        .frame_height = 480,
        .box_corners =
            std::array{
                mqtt::PixelPoint{ .x = 100.0, .y = 50.0 },
                mqtt::PixelPoint{ .x = 300.0, .y = 50.0 },
                mqtt::PixelPoint{ .x = 300.0, .y = 150.0 },
                mqtt::PixelPoint{ .x = 100.0, .y = 150.0 },
            },
        .barcode = std::move(barcode),
        .barcode_region_detected = barcode_region_detected,
    };
}

mqtt::MqttMessage WorkCreated(std::string work_id = std::string(kWorkId)) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "WORK-ASSIGN-01",
        .message_type = mqtt::MessageType::kWorkCreated,
        .source_id = "central-server",
        .timestamp = "2026-07-21T11:00:01Z",
        .data = mqtt::WorkCreatedPayload{ .work_id = std::move(work_id) },
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
    assert(workflow.NeedsBarcodeFallback());
    assert(workflow.AssignWork(WorkCreated()));
    // Re-delivery after a transient WORK_ASSIGNED status publish failure is idempotent.
    assert(workflow.AssignWork(WorkCreated()));
    assert(!workflow.HasPendingBarcode());
    assert(workflow.NeedsBarcodeFallback());
    assert(!workflow.Observe(Observation(std::string("8801234567893")), "IGNORED", "2026-07-21T11:00:01Z").has_value());
    assert(workflow.HasPendingBarcode());
    assert(!workflow.NeedsBarcodeFallback());
    const auto assigned = workflow.TakeAssignedWork();
    assert(assigned.has_value());
    assert(assigned->observation.has_value());
    assert(assigned->observation->barcode == "8801234567893");

    const auto position =
        vision::MakePositionDetectedMessage("PI-VISION-01", *assigned, "MSG-POSITION-01", "2026-07-21T11:00:02Z");
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *assigned, "MSG-BARCODE-01", "2026-07-21T11:00:02Z");
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), position).IsSuccess());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), barcode).IsSuccess());
    const auto* position_payload = mqtt::GetPayload<mqtt::PositionDetectedPayload>(position);
    assert(position_payload != nullptr && position_payload->box_corners.has_value());
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
    assert(!workflow.AssignWork(WorkCreated()));
}

void TestSensorWorkCanBeAssignedBeforeVisionDetection() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 2, 1);
    assert(workflow.AssignWork(WorkCreated()));
    assert(!workflow.TakeAssignedWork().has_value());

    assert(!workflow.Observe(Observation(), "MSG-VISION-BOX-01", "2026-07-21T11:00:01Z").has_value());
    // The sensor already created the work, so vision must attach its observation
    // without publishing a second BOX_DETECTED/work.
    assert(
        !workflow.Observe(Observation("8801234567893", true), "MSG-VISION-BOX-02", "2026-07-21T11:00:02Z").has_value());
    const auto assigned = workflow.TakeAssignedWork();
    assert(assigned.has_value());
    assert(assigned->work_id == kWorkId);
    assert(assigned->observation.has_value());
    assert(assigned->observation->barcode == "8801234567893");
}

void TestBarcodeSurvivesDetectionConfirmation() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 3, 1);
    assert(!workflow.Observe(Observation("8801234567893", true), "MSG-BOX-00", "2026-07-21T11:00:00Z").has_value());
    assert(!workflow.Observe(Observation(), "MSG-BOX-01", "2026-07-21T11:00:01Z").has_value());
    assert(workflow.Observe(Observation(), "MSG-BOX-02", "2026-07-21T11:00:02Z").has_value());
    assert(workflow.AssignWork(WorkCreated()));

    const auto work = workflow.TakeAssignedWork();
    assert(work.has_value());
    assert(work->observation.has_value());
    assert(work->observation->barcode == "8801234567893");
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *work, "MSG-BARCODE-01", "2026-07-21T11:00:03Z");
    const auto* payload = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(barcode);
    assert(payload != nullptr);
    assert(payload->recognition_status == "SUCCESS");
    assert(!payload->error_code.has_value());
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
    assert(payload->error_code == "ERR-VISION-BARCODE-REGION-NOT-DETECTED");
    assert(payload->failure_stage == "BARCODE_DETECTION");
}

void TestDefaultBarcodeWaitRetriesBeyondLegacyLimit() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 1, 1);
    assert(workflow.Observe(Observation(), "MSG-BOX-01", "2026-07-21T11:00:00Z").has_value());
    assert(workflow.AssignWork(WorkCreated()));

    for (int frame = 0; frame < 90; ++frame) {
        assert(!workflow.Observe(Observation(), "IGNORED", "2026-07-21T11:00:01Z").has_value());
    }
    assert(!workflow.TakeAssignedWork().has_value());

    assert(!workflow.Observe(Observation("8801234567893", true), "IGNORED", "2026-07-21T11:00:02Z").has_value());
    const auto work = workflow.TakeAssignedWork();
    assert(work.has_value());
    assert(work->observation.has_value());
    assert(work->observation->barcode == "8801234567893");
}

void TestBarcodeDecodeFailureIdentifiesStage() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 1, 1, 1);
    assert(workflow.Observe(Observation(std::nullopt, true), "MSG-BOX-01", "2026-07-21T11:00:00Z").has_value());
    assert(workflow.AssignWork(WorkCreated()));
    assert(!workflow.Observe(Observation(std::nullopt, true), "IGNORED", "2026-07-21T11:00:01Z").has_value());
    const auto work = workflow.TakeAssignedWork();
    assert(work.has_value());

    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *work, "MSG-BARCODE-FAIL", "2026-07-21T11:00:02Z");
    const auto* payload = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(barcode);
    assert(payload != nullptr);
    assert(payload->recognition_status == "FAILED");
    assert(payload->error_code == "ERR-VISION-BARCODE-DECODE-FAILED");
    assert(payload->failure_stage == "BARCODE_DECODE");
}

void TestPreassignmentTimeoutProducesBoxDetectionFailure() {
    constexpr std::string_view kNextWorkId = "7026045c-92ba-4fd9-93dc-6dfa04a5fd30";
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 2, 1, 1, 3);
    assert(workflow.AssignWork(WorkCreated()));
    assert(!workflow.Observe(std::nullopt, "IGNORED-01", "2026-07-21T11:00:01Z").has_value());
    assert(!workflow.TakeAssignedWork().has_value());
    assert(!workflow.Observe(std::nullopt, "IGNORED-02", "2026-07-21T11:00:02Z").has_value());
    assert(!workflow.TakeAssignedWork().has_value());
    assert(!workflow.Observe(std::nullopt, "IGNORED-03", "2026-07-21T11:00:03Z").has_value());

    const auto work = workflow.TakeAssignedWork();
    assert(work.has_value());
    assert(work->work_id == kWorkId);
    assert(!work->observation.has_value());
    bool position_rejected = false;
    try {
        static_cast<void>(
            vision::MakePositionDetectedMessage("PI-VISION-01", *work, "IGNORED-POSITION", "2026-07-21T11:00:03Z"));
    } catch (const std::bad_optional_access&) {
        position_rejected = true;
    }
    assert(position_rejected);
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *work, "MSG-BOX-TIMEOUT", "2026-07-21T11:00:03Z");
    const auto* payload = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(barcode);
    assert(payload != nullptr);
    assert(payload->recognition_status == "FAILED");
    assert(payload->error_code == "ERR-VISION-BOX-NOT-DETECTED");
    assert(payload->failure_stage == "BOX_DETECTION");

    workflow.CompleteWork();
    assert(!workflow.Observe(std::nullopt, "IGNORED-CLEAR", "2026-07-21T11:00:04Z").has_value());
    assert(workflow.AssignWork(WorkCreated(std::string(kNextWorkId))));
}

void TestPreassignmentTimeoutDiscardsUnconfirmedObservation() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 2, 1, 1, 2);
    assert(workflow.AssignWork(WorkCreated()));
    assert(!workflow.Observe(Observation(), "IGNORED-BOX", "2026-07-21T11:00:01Z").has_value());
    assert(!workflow.Observe(std::nullopt, "IGNORED-01", "2026-07-21T11:00:02Z").has_value());
    assert(!workflow.Observe(std::nullopt, "IGNORED-02", "2026-07-21T11:00:03Z").has_value());

    const auto work = workflow.TakeAssignedWork();
    assert(work.has_value());
    assert(!work->observation.has_value());
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *work, "MSG-BOX-TIMEOUT", "2026-07-21T11:00:03Z");
    const auto* payload = mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(barcode);
    assert(payload != nullptr);
    assert(payload->error_code == "ERR-VISION-BOX-NOT-DETECTED");
    assert(payload->failure_stage == "BOX_DETECTION");
}

void TestResultOutboxRetriesFromFirstUnsentPublication() {
    const auto work = vision::AssignedVisionWork{ .work_id = std::string(kWorkId),
                                                  .observation = Observation("8801234567893", true) };
    const auto position =
        vision::MakePositionDetectedMessage("PI-VISION-01", work, "MSG-POSITION-01", "2026-07-21T11:00:02Z");
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", work, "MSG-BARCODE-01", "2026-07-21T11:00:02Z");
    vision::VisionResultOutbox outbox;
    assert(outbox.Enqueue(std::string(kWorkId), {
                                                    { vision::VisionPublicationChannel::kEvent, position },
                                                    { vision::VisionPublicationChannel::kError, barcode },
                                                }));
    assert(outbox.PendingWorkId() == kWorkId);

    int event_attempts = 0;
    int error_attempts = 0;
    const auto event_publisher = [&event_attempts](const mqtt::MqttMessage&) {
        ++event_attempts;
        return true;
    };
    const auto error_publisher = [&error_attempts](const mqtt::MqttMessage&) {
        ++error_attempts;
        return error_attempts > 1;
    };
    assert(!outbox.Flush(event_publisher, error_publisher));
    assert(outbox.PendingWorkId() == kWorkId);
    assert(event_attempts == 1);
    assert(error_attempts == 1);

    assert(outbox.Flush(event_publisher, error_publisher));
    assert(!outbox.PendingWorkId().has_value());
    assert(event_attempts == 1);
    assert(error_attempts == 2);
}

void TestVisionControlLifecycle() {
    device::DeviceControlState control({
        .device_id = "PI-VISION-01",
        .component_name = "vision",
        .not_ready_error_code = "ERR-CAMERA-UNAVAILABLE",
    });
    assert(control.State() == device::DeviceOperatingState::kStopped);
    assert(!control.IsOperational());

    control.SetReady(false);
    assert(control.State() == device::DeviceOperatingState::kError);
    auto decision = Handle(control, ControlCommand(mqtt::ControlCommand::kRecovery), 1);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kProcessing);
    assert(control.ConsumeResetRequest());
    control.SetReady(true);
    assert(control.CompleteRecovery("MSG-INITIAL-RECOVERY", "2026-07-21T11:00:00Z").has_value());
    assert(control.State() == device::DeviceOperatingState::kStopped);

    control.SetReady(false);
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 2);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kRejected);
    assert(ResponsePayload(decision).error_code == "ERR-INVALID-STATE");

    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kRecovery), 3);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kProcessing);
    assert(control.ConsumeResetRequest());
    control.SetReady(true);
    assert(control.CompleteRecovery("MSG-READY-RECOVERY", "2026-07-21T11:00:01Z").has_value());
    assert(control.State() == device::DeviceOperatingState::kStopped);
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 4);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(decision.state_changed);
    assert(control.State() == device::DeviceOperatingState::kRunning);
    assert(control.IsOperational());

    control.SetFault();
    assert(control.State() == device::DeviceOperatingState::kError);
    assert(!control.IsOperational());
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kRecovery), 5);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kProcessing);
    assert(control.ConsumeResetRequest());
    control.SetReady(true);
    assert(control.CompleteRecovery("MSG-FAULT-RECOVERY", "2026-07-21T11:00:01Z").has_value());
    static_cast<void>(Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 6));

    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStop), 7);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(!decision.clear_work);
    assert(control.State() == device::DeviceOperatingState::kStopped);

    static_cast<void>(Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 8));
    decision = Handle(control, EmergencyStop(), 9);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(!decision.clear_work);
    assert(control.State() == device::DeviceOperatingState::kEmergencyStop);

    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 10);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kRejected);

    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kRecovery), 11);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kProcessing);
    assert(decision.clear_work);
    assert(control.State() == device::DeviceOperatingState::kRecovering);
    assert(control.ConsumeResetRequest());
    assert(!control.ConsumeResetRequest());

    control.SetReady(false);
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kInitialize), 12);
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
    assert(control.State() == device::DeviceOperatingState::kStopped);
    assert(!control.CompleteRecovery("IGNORED", "2026-07-21T11:00:04Z").has_value());
    decision = Handle(control, ControlCommand(mqtt::ControlCommand::kInitialize), 13);
    assert(ResponsePayload(decision).result == mqtt::CommandResult::kSuccess);
    assert(control.State() == device::DeviceOperatingState::kStopped);

    static_cast<void>(Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 14));
    control.SetReady(false);
    assert(control.State() == device::DeviceOperatingState::kError);
    assert(!control.IsOperational());
}

void TestStopPreservesPendingVisionWork() {
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
    assert(!decision.clear_work);
    assert(workflow.TakeAssignedWork().has_value());
    assert(!control.IsOperational());
}

void TestSafetyRecoveryClearsPendingVisionWork() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 1, 1);
    vision::VisionResultOutbox outbox;
    device::DeviceControlState control({
        .device_id = "PI-VISION-01",
        .component_name = "vision",
        .not_ready_error_code = "ERR-CAMERA-UNAVAILABLE",
    });
    control.SetReady(true);
    static_cast<void>(Handle(control, ControlCommand(mqtt::ControlCommand::kStart), 1));
    assert(workflow.Observe(Observation("8801234567893"), "MSG-BOX-RECOVERY", "2026-07-21T11:00:00Z").has_value());
    assert(workflow.AssignWork(WorkCreated()));
    assert(outbox.Enqueue(std::string(kWorkId), { { vision::VisionPublicationChannel::kEvent, WorkCreated() } }));

    static_cast<void>(Handle(control, EmergencyStop(), 2));
    const auto recovery = Handle(control, ControlCommand(mqtt::ControlCommand::kRecovery), 3);
    assert(recovery.clear_work);
    if (recovery.clear_work) {
        workflow.Reset();
        outbox.Reset();
    }

    assert(!workflow.TakeAssignedWork().has_value());
    assert(!outbox.PendingWorkId().has_value());
    assert(control.ConsumeResetRequest());
}

}  // namespace

int main() {
    TestDetectionAssignmentAndResultMessages();
    TestSensorWorkCanBeAssignedBeforeVisionDetection();
    TestBarcodeSurvivesDetectionConfirmation();
    TestMissingBarcodeProducesFailedResult();
    TestDefaultBarcodeWaitRetriesBeyondLegacyLimit();
    TestBarcodeDecodeFailureIdentifiesStage();
    TestPreassignmentTimeoutProducesBoxDetectionFailure();
    TestPreassignmentTimeoutDiscardsUnconfirmedObservation();
    TestResultOutboxRetriesFromFirstUnsentPublication();
    TestVisionControlLifecycle();
    TestStopPreservesPendingVisionWork();
    TestSafetyRecoveryClearsPendingVisionWork();
    return 0;
}
