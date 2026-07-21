#include "vision_mqtt_workflow.hpp"

#include <cassert>
#include <optional>
#include <string>

#include "logistics/contracts/mqtt_validation.hpp"

namespace {

namespace mqtt = logistics::contracts::mqtt;
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

void TestDetectionAssignmentAndResultMessages() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 2, 2);
    assert(!workflow.Observe(Observation(), "MSG-BOX-00", "2026-07-21T11:00:00Z").has_value());
    const auto box = workflow.Observe(Observation(), "MSG-BOX-01", "2026-07-21T11:00:00Z");
    assert(box.has_value());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), *box).IsSuccess());

    assert(!workflow.Observe(Observation(std::string("8801234567893")), "IGNORED", "2026-07-21T11:00:01Z").has_value());
    assert(workflow.AssignWork(WorkCreated()));
    const auto assigned = workflow.TakeAssignedWork();
    assert(assigned.has_value());
    assert(assigned->observation.barcode == "8801234567893");

    const auto position =
        vision::MakePositionDetectedMessage("PI-VISION-01", *assigned, "MSG-POSITION-01", "2026-07-21T11:00:02Z");
    const auto barcode =
        vision::MakeBarcodeDetectedMessage("PI-VISION-01", *assigned, "MSG-BARCODE-01", "2026-07-21T11:00:02Z");
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), position).IsSuccess());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceEventTopic("PI-VISION-01"), barcode).IsSuccess());

    workflow.CompleteWork();
    assert(!workflow.Observe(std::nullopt, "IGNORED", "2026-07-21T11:00:03Z").has_value());
    assert(!workflow.Observe(std::nullopt, "IGNORED", "2026-07-21T11:00:04Z").has_value());
    assert(!workflow.Observe(Observation(), "MSG-BOX-02", "2026-07-21T11:00:05Z").has_value());
    assert(workflow.Observe(Observation(), "MSG-BOX-03", "2026-07-21T11:00:06Z").has_value());
}

void TestMissingBarcodeProducesFailedResult() {
    vision::VisionMqttWorkflow workflow("PI-VISION-01", 1, 1);
    assert(workflow.Observe(Observation(), "MSG-BOX-01", "2026-07-21T11:00:00Z").has_value());
    assert(workflow.AssignWork(WorkCreated()));
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

}  // namespace

int main() {
    TestDetectionAssignmentAndResultMessages();
    TestMissingBarcodeProducesFailedResult();
    return 0;
}
