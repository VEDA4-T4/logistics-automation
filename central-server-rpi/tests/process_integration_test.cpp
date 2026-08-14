#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logistics/central_server/command_manager.hpp"
#include "logistics/central_server/database.hpp"
#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/mqtt_handler.hpp"
#include "logistics/central_server/persistence.hpp"
#include "logistics/central_server/process_orchestrator.hpp"
#include "logistics/central_server/sensor_detection.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

#ifndef LOGISTICS_TEST_MIGRATION_DIR
#define LOGISTICS_TEST_MIGRATION_DIR "central-server-rpi/db/migrations"
#endif

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

constexpr std::string_view kTimestamp = "2026-08-10T03:00:00Z";
constexpr std::string_view kInputId = "PI-INPUT-01";
constexpr std::string_view kVisionId = "PI-VISION-01";
constexpr std::string_view kGripperId = "PI-GRIPPER-01";
constexpr std::string_view kSortingId = "PI-SORTING-01";
constexpr std::string_view kLineTracerId = "PI-LT-01";

struct PublishedMessage final {
    std::string topic;
    mqtt::MqttMessage message;
};

[[nodiscard]] std::string Encode(const mqtt::MqttMessage& message) {
    const auto encoded = mqtt::SerializeMessage(message);
    assert(encoded.IsSuccess());
    return encoded.payload;
}

[[nodiscard]] central_server::ProcessOrchestratorConfig ProcessConfig() {
    return {
        .enabled = true,
        .server_id = "central-server",
        .input_device_id = std::string(kInputId),
        .vision_device_id = std::string(kVisionId),
        .gripper_device_id = std::string(kGripperId),
        .sorting_device_id = std::string(kSortingId),
        .line_tracer_device_id = std::string(kLineTracerId),
        .line_tracer_initial_position = "A",
        .default_destination = "3",
        .homography = { .enabled = false },
    };
}

class ProcessIntegrationHarness final {
public:
    ProcessIntegrationHarness()
        : root_(std::filesystem::temp_directory_path() /
                ("logistics-process-integration-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
          orchestrator_(ProcessConfig()) {
        std::filesystem::create_directories(root_);
        const central_server::DatabaseConfig database_config{
            .path = root_ / "process.db",
            .migration_dir = LOGISTICS_TEST_MIGRATION_DIR,
            .busy_timeout_ms = 100,
        };
        assert(database_.Open(database_config).ok());
        assert(central_server::MigrationRunner::Apply(database_, database_config.migration_dir).ok());

        central_server::StorageConfig storage;
        storage.image_root = root_ / "images";
        persistence_ = std::make_unique<central_server::PersistenceService>(database_, storage);
        handler_ = std::make_unique<central_server::MqttHandler>(device_manager_, central_server::MqttHandler::Logger{},
                                                                 persistence_.get(), "3");
        ConfigureHandlers();
        RegisterRequiredNodes();
    }

    ~ProcessIntegrationHarness() {
        handler_.reset();
        persistence_.reset();
        static_cast<void>(database_.Close());
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    ProcessIntegrationHarness(const ProcessIntegrationHarness&) = delete;
    ProcessIntegrationHarness& operator=(const ProcessIntegrationHarness&) = delete;

    void FailPublishingTo(std::string device_id) {
        failed_target_ = std::move(device_id);
    }

    [[nodiscard]] const std::string& WorkId() const noexcept {
        return work_id_;
    }

    [[nodiscard]] const central_server::ProcessOrchestrator& Orchestrator() const noexcept {
        return orchestrator_;
    }

    [[nodiscard]] bool DetectBox() {
        return Handle(mqtt::DeviceEventTopic(kInputId),
                      Message(mqtt::MessageType::kBoxDetected, kInputId,
                              mqtt::BoxDetectedPayload{ .detected = true, .image_name = "integration-box.jpg" }));
    }

    [[nodiscard]] bool DetectPosition() {
        return Handle(mqtt::DeviceEventTopic(kVisionId), Message(mqtt::MessageType::kPositionDetected, kVisionId,
                                                                 mqtt::PositionDetectedPayload{
                                                                     .work_id = work_id_,
                                                                     .box_x = 100,
                                                                     .box_y = 50,
                                                                     .box_width = 200,
                                                                     .box_height = 100,
                                                                     .center_x = 200,
                                                                     .center_y = 100,
                                                                     .offset_x = 0,
                                                                     .offset_y = 0,
                                                                     .position_status = "DETECTED",
                                                                     .box_corners = std::nullopt,
                                                                 }));
    }

    [[nodiscard]] bool DetectBarcode() {
        return Handle(mqtt::DeviceEventTopic(kVisionId), Message(mqtt::MessageType::kBarcodeDetected, kVisionId,
                                                                 mqtt::BarcodeDetectedPayload{
                                                                     .work_id = work_id_,
                                                                     .recognition_status = "SUCCESS",
                                                                     .barcode = "0000000000000",
                                                                     .confidence = 0.99,
                                                                     .message = std::nullopt,
                                                                     .error_code = std::nullopt,
                                                                     .failure_stage = std::nullopt,
                                                                 }));
    }

    [[nodiscard]] bool ReportStatus(std::string_view device_id, std::string state) {
        return Handle(mqtt::DeviceStatusTopic(device_id), Message(mqtt::MessageType::kDeviceStatus, device_id,
                                                                  mqtt::DeviceStatusPayload{
                                                                      .status = mqtt::ConnectionState::kOnline,
                                                                      .current_state = std::move(state),
                                                                      .job_id = work_id_,
                                                                      .error_code = std::nullopt,
                                                                      .departure_position = std::nullopt,
                                                                      .target_position = std::nullopt,
                                                                      .confirmed_position = std::nullopt,
                                                                      .movement_state = std::nullopt,
                                                                  }));
    }

    [[nodiscard]] bool DetectSortedProduct(std::int32_t sensor_id = 3) {
        for (int reading = 0; reading < 3; ++reading) {
            if (!Handle(mqtt::DeviceEventTopic(kSortingId), Message(mqtt::MessageType::kSensorStatus, kSortingId,
                                                                    mqtt::SensorStatusPayload{
                                                                        .sensor_id = sensor_id,
                                                                        .measurement_status = "OK",
                                                                        .distance_cm = 5,
                                                                        .detection_status = std::nullopt,
                                                                    }))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool CompleteWork() {
        return Handle(mqtt::DeviceEventTopic(kLineTracerId), Message(mqtt::MessageType::kWorkCompleted, kLineTracerId,
                                                                     mqtt::WorkCompletedPayload{
                                                                         .work_id = work_id_,
                                                                         .result = "SUCCESS",
                                                                         .message = std::string("transport completed"),
                                                                     }));
    }

    [[nodiscard]] std::size_t CountControlCommands(std::string_view target, mqtt::ControlCommand command) const {
        return static_cast<std::size_t>(
            std::count_if(published_.begin(), published_.end(), [target, command](const PublishedMessage& publication) {
                const auto* payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(publication.message);
                return payload != nullptr && payload->target_device_id == target && payload->command == command;
            }));
    }

    [[nodiscard]] std::vector<std::string> DeviceCommandTargets() const {
        std::vector<std::string> targets;
        for (const auto& publication : published_) {
            const auto parsed = mqtt::ParseTopic(publication.topic);
            if (parsed.kind == mqtt::TopicKind::kDeviceCommand) {
                targets.emplace_back(parsed.endpoint_id);
            }
        }
        return targets;
    }

    [[nodiscard]] bool ReportCommandResult(std::string_view target, mqtt::CommandResult result) {
        const auto publication =
            std::ranges::find_if(published_.rbegin(), published_.rend(), [target](const PublishedMessage& candidate) {
                const auto parsed = mqtt::ParseTopic(candidate.topic);
                return parsed.kind == mqtt::TopicKind::kDeviceCommand && parsed.endpoint_id == target;
            });
        if (publication == published_.rend()) {
            return false;
        }

        std::string request_id;
        mqtt::ControlCommand command{ mqtt::ControlCommand::kUnknown };
        if (const auto* control = mqtt::GetPayload<mqtt::ControlCommandPayload>(publication->message)) {
            request_id = control->request_id;
            command = control->command;
        } else if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(publication->message)) {
            request_id = destination->request_id;
            command = destination->command;
        }
        return Handle(mqtt::DeviceResponseTopic(target),
                      Message(mqtt::MessageType::kCommandResponse, target,
                              mqtt::CommandResponsePayload{
                                  .request_id = std::move(request_id),
                                  .command = command,
                                  .result = result,
                                  .error_code = result == mqtt::CommandResult::kSuccess
                                                    ? std::nullopt
                                                    : std::optional<std::string>{ "ERR-INTEGRATION-COMMAND" },
                                  .message = result == mqtt::CommandResult::kSuccess ? "integration command completed"
                                                                                     : "integration command failure",
                              }));
    }

private:
    template <typename Payload>
    [[nodiscard]] mqtt::MqttMessage Message(mqtt::MessageType type, std::string_view source_id, Payload payload) {
        return {
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "INTEGRATION-" + std::to_string(++message_sequence_),
            .message_type = type,
            .source_id = std::string(source_id),
            .timestamp = std::string(kTimestamp),
            .data = std::move(payload),
        };
    }

    [[nodiscard]] bool Handle(std::string topic, const mqtt::MqttMessage& message) {
        return handler_->Handle(topic, Encode(message), kTimestamp);
    }

    [[nodiscard]] bool Publish(std::string topic, const mqtt::MqttMessage& message) {
        const auto parsed = mqtt::ParseTopic(topic);
        if (parsed.kind == mqtt::TopicKind::kDeviceCommand && parsed.endpoint_id == failed_target_) {
            return false;
        }
        assert(mqtt::ValidateTopicMessage(topic, message).IsSuccess());
        published_.push_back({ .topic = std::move(topic), .message = message });
        return true;
    }

    [[nodiscard]] static std::string RequestId(const mqtt::MqttMessage& message) {
        if (const auto* control = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
            return control->request_id;
        }
        if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
            return destination->request_id;
        }
        return {};
    }

    [[nodiscard]] bool DispatchCommand(const mqtt::MqttMessage& message) {
        const auto route = central_server::ResolveCommandTargets(message, device_manager_.RegisteredDevices());
        if (!route.IsValid()) {
            return false;
        }
        if (!command_manager_.TrackCommand(message, route.target_device_ids)) {
            return true;
        }

        std::vector<std::string> failed_devices;
        for (const auto& target : route.target_device_ids) {
            const auto forwarded = central_server::PrepareCommandForDevice(
                message, target, kLineTracerId, ProcessConfig().line_tracer_initial_position);
            if (!Publish(mqtt::DeviceCommandTopic(target), forwarded)) {
                failed_devices.push_back(target);
            }
        }
        if (failed_devices.empty()) {
            return true;
        }
        static_cast<void>(command_manager_.HandleDispatchFailures(RequestId(message), failed_devices, kTimestamp));
        return false;
    }

    [[nodiscard]] bool HandleProcessMessage(const mqtt::MqttMessage& message) {
        if (const auto work_id = sorting_detection_gate_.ShouldStop(
                message, orchestrator_.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning,
                orchestrator_.StateMachine().ActiveWorks())) {
            const auto commands = orchestrator_.SortingDetectionCommands(*work_id, message.timestamp);
            if (commands.empty() || !DispatchProcessCommands(commands)) {
                sorting_detection_gate_.Retry();
                return false;
            }
        }
        const auto result = orchestrator_.Handle(message);
        if (!result.handled) {
            return true;
        }
        if (result.transition.disposition == central_server::TransitionDisposition::kRejected) {
            return false;
        }
        return DispatchProcessCommands(result.commands);
    }

    [[nodiscard]] bool DispatchProcessCommands(const std::vector<central_server::ProcessCommandIntent>& commands) {
        for (const auto& intent : commands) {
            if (!central_server::ResolveCommandTargets(intent.message, device_manager_.RegisteredDevices()).IsValid()) {
                static_cast<void>(orchestrator_.FailDispatch(intent, "target process node is unavailable"));
                return false;
            }
            if (!DispatchCommand(intent.message)) {
                static_cast<void>(orchestrator_.FailDispatch(intent, "process command publication failed"));
                return false;
            }
            if (!orchestrator_.ConfirmDispatch(intent).Applied()) {
                return false;
            }
            if (!process_command_tracker_.Track(intent)) {
                return false;
            }
        }
        return true;
    }

    void ConfigureHandlers() {
        handler_->SetProcessMessageGuard([this](const mqtt::MqttMessage& message) {
            const auto preview = orchestrator_.Preview(message);
            return !preview.handled ||
                   preview.transition.disposition != central_server::TransitionDisposition::kRejected;
        });
        handler_->SetProcessMessageHandler(
            [this](const mqtt::MqttMessage& message) { return HandleProcessMessage(message); });
        handler_->SetWorkCreatedHandler([this](std::string_view device_id, std::string_view work_id) {
            work_id_ = work_id;
            const mqtt::MqttMessage created{
                .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                .message_id = "WORK-" + work_id_,
                .message_type = mqtt::MessageType::kWorkCreated,
                .source_id = "central-server",
                .timestamp = std::string(kTimestamp),
                .data = mqtt::WorkCreatedPayload{ .work_id = work_id_ },
            };
            const auto begin = orchestrator_.BeginWork(created.message_id, work_id_, device_id, created.timestamp);
            if (!begin.transition.Applied() || !DispatchProcessCommands(begin.commands)) {
                return false;
            }
            const bool sent_to_vision = Publish(mqtt::DeviceCommandTopic(kVisionId), created);
            const bool sent_to_qt = Publish(mqtt::QtEventTopic("control-center"), created);
            return sent_to_vision && sent_to_qt &&
                   orchestrator_.ConfirmVisionAssignment(created.message_id, work_id_).Applied();
        });
        handler_->SetQtEventHandler([this](const mqtt::MqttMessage& message) {
            return Publish(mqtt::QtEventTopic("control-center"), message);
        });
        handler_->SetQtResponseHandler([this](const mqtt::MqttMessage& message) {
            const auto completed_intent = process_command_tracker_.HandleResponse(message);
            if (!completed_intent.has_value()) {
                return true;
            }
            const auto* response = mqtt::GetPayload<mqtt::CommandResponsePayload>(message);
            if (response != nullptr && (response->result == mqtt::CommandResult::kSuccess ||
                                        response->result == mqtt::CommandResult::kDuplicated)) {
                const auto completion = orchestrator_.HandleCommandCompletion(*completed_intent, message);
                return !completion.handled ||
                       (completion.transition.disposition != central_server::TransitionDisposition::kRejected &&
                        DispatchProcessCommands(completion.commands));
            }
            return orchestrator_
                .FailDispatch(*completed_intent, response == nullptr ? "process command failed" : response->message)
                .Applied();
        });
    }

    void RegisterRequiredNodes() {
        RegisterNode(kInputId, "input", true);
        RegisterNode(kVisionId, "vision", false);
        RegisterNode(kGripperId, "gripper", true);
        RegisterNode(kSortingId, "sorting", true);
        RegisterNode(kLineTracerId, "linetracer", true);
        assert(device_manager_.RegisteredDeviceCount() == 5);
    }

    void RegisterNode(std::string_view device_id, std::string device_type, bool uart_connected) {
        const auto registration = Message(mqtt::MessageType::kDeviceRegister, device_id,
                                          mqtt::DeviceRegisterPayload{
                                              .device_type = std::move(device_type),
                                              .node_name = std::string(device_id) + "-node",
                                              .status = mqtt::ConnectionState::kOnline,
                                              .ip_address = "192.168.10.10",
                                              .uart_connected = uart_connected,
                                          });
        assert(Handle(mqtt::DeviceRegisterTopic(device_id), registration));
    }

    std::filesystem::path root_;
    central_server::Database database_;
    std::unique_ptr<central_server::PersistenceService> persistence_;
    central_server::DeviceManager device_manager_;
    central_server::CommandManager command_manager_;
    central_server::ProcessOrchestrator orchestrator_;
    central_server::SortingDetectionGate sorting_detection_gate_{ std::string(kSortingId) };
    central_server::ProcessCommandTracker process_command_tracker_;
    std::unique_ptr<central_server::MqttHandler> handler_;
    std::vector<PublishedMessage> published_;
    std::string failed_target_;
    std::string work_id_;
    std::uint64_t message_sequence_{};
};

void AdvanceToGripperTransfer(ProcessIntegrationHarness& harness) {
    assert(harness.DetectBox());
    assert(mqtt::IsValidUuid(harness.WorkId()));
    assert(harness.DetectPosition());
    assert(harness.DetectBarcode());
    assert(harness.ReportStatus(kGripperId, "TRANSFERRING"));
}

void TestAutomaticProcessCompletesThroughAllNodes() {
    ProcessIntegrationHarness harness;
    AdvanceToGripperTransfer(harness);
    assert(harness.ReportCommandResult(kGripperId, mqtt::CommandResult::kSuccess));
    assert(harness.ReportStatus(kSortingId, "ROUTING"));
    assert(harness.DetectSortedProduct());
    assert(harness.ReportStatus(kSortingId, "CYCLE_COMPLETE"));
    assert(harness.ReportStatus(kLineTracerId, "FOLLOWING_LINE"));
    assert(harness.CompleteWork());

    assert(harness.CountControlCommands(kInputId, mqtt::ControlCommand::kStop) == 1);
    assert(harness.CountControlCommands(kInputId, mqtt::ControlCommand::kStart) == 1);
    assert(harness.CountControlCommands(kSortingId, mqtt::ControlCommand::kStart) == 1);
    assert(harness.CountControlCommands(kSortingId, mqtt::ControlCommand::kStop) == 1);
    assert(harness.CountControlCommands(kSortingId, mqtt::ControlCommand::kRecovery) == 1);
    assert((harness.DeviceCommandTargets() ==
            std::vector<std::string>{ std::string(kInputId), std::string(kVisionId), std::string(kLineTracerId),
                                      std::string(kGripperId), std::string(kSortingId), std::string(kInputId),
                                      std::string(kSortingId), std::string(kSortingId), std::string(kSortingId) }));

    const auto work = harness.Orchestrator().StateMachine().FindWork(harness.WorkId());
    assert(work.has_value());
    assert(work->stage == central_server::WorkStage::kCompleted);
}

void TestSortingDispatchFailureKeepsInputStopped() {
    ProcessIntegrationHarness harness;
    AdvanceToGripperTransfer(harness);
    harness.FailPublishingTo(std::string(kSortingId));
    assert(!harness.ReportStatus(kGripperId, "COMPLETED"));

    assert(harness.CountControlCommands(kInputId, mqtt::ControlCommand::kStop) == 1);
    assert(harness.CountControlCommands(kInputId, mqtt::ControlCommand::kStart) == 0);
    const auto work = harness.Orchestrator().StateMachine().FindWork(harness.WorkId());
    assert(work.has_value());
    assert(work->stage == central_server::WorkStage::kFailed);
}

void TestRejectedGripperCommandFailsWork() {
    ProcessIntegrationHarness harness;
    assert(harness.DetectBox());
    assert(harness.DetectPosition());
    assert(harness.DetectBarcode());
    assert(harness.ReportCommandResult(kGripperId, mqtt::CommandResult::kRejected));

    const auto work = harness.Orchestrator().StateMachine().FindWork(harness.WorkId());
    assert(work.has_value());
    assert(work->stage == central_server::WorkStage::kFailed);
}

void TestTimedOutGripperCommandFailsWork() {
    ProcessIntegrationHarness harness;
    assert(harness.DetectBox());
    assert(harness.DetectPosition());
    assert(harness.DetectBarcode());
    assert(harness.ReportCommandResult(kGripperId, mqtt::CommandResult::kTimeout));

    const auto work = harness.Orchestrator().StateMachine().FindWork(harness.WorkId());
    assert(work.has_value());
    assert(work->stage == central_server::WorkStage::kFailed);
}

}  // namespace

int main() {
    TestAutomaticProcessCompletesThroughAllNodes();
    TestSortingDispatchFailureKeepsInputStopped();
    TestRejectedGripperCommandFailsWork();
    TestTimedOutGripperCommandFailsWork();
    return 0;
}
