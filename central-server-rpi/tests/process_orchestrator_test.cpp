#include "logistics/central_server/process_orchestrator.hpp"

#include <array>
#include <cassert>
#include <optional>
#include <string>
#include <unordered_map>

#include "logistics/contracts/mqtt_validation.hpp"

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

constexpr auto kWorkId = "d8e9b2be-bfc0-471c-9000-590123412345";
constexpr auto kQueuedWorkId = "a8e9b2be-bfc0-471c-9000-590123412346";
constexpr auto kReplacementWorkId = "97c42b78-9299-4a3b-85aa-0f959954ea73";
constexpr auto kTimestamp = "2026-07-25T01:00:00Z";

mqtt::MqttMessage Message(std::string message_id, mqtt::MessageType type, std::string source_id,
                          mqtt::MessagePayload payload) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = type,
        .source_id = std::move(source_id),
        .timestamp = kTimestamp,
        .data = std::move(payload),
    };
}

mqtt::MqttMessage Status(std::string id, std::string source, std::string state,
                         mqtt::ConnectionState connection = mqtt::ConnectionState::kOnline,
                         std::optional<std::string> work_id = std::string(kWorkId), bool position_reset = false) {
    return Message(std::move(id), mqtt::MessageType::kDeviceStatus, std::move(source),
                   mqtt::DeviceStatusPayload{
                       .status = connection,
                       .current_state = std::move(state),
                       .job_id = std::move(work_id),
                       .error_code = std::nullopt,
                       .position_reset = position_reset,
                   });
}

mqtt::MqttMessage Heartbeat(std::string id, std::string source, std::string state) {
    return Message(std::move(id), mqtt::MessageType::kHeartbeat, std::move(source),
                   mqtt::HeartbeatPayload{
                       .status = mqtt::ConnectionState::kOnline,
                       .current_state = std::move(state),
                       .uptime = 1,
                       .job_id = std::nullopt,
                       .error_code = std::nullopt,
                   });
}

void TestEventFlowCreatesCommandsForEachNode() {
    central_server::ProcessOrchestrator orchestrator({
        .enabled = true,
        .server_id = "central-server",
        .input_device_id = "PI-INPUT-01",
        .vision_device_id = "PI-VISION-01",
        .gripper_device_id = "PI-GRIPPER-01",
        .sorting_device_id = "PI-SORTING-01",
        .line_tracer_device_id = "PI-LT-01",
        .line_tracer_initial_position = {},
        .homography =
            {
                .enabled = true,
                .pixel_to_conveyor = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 },
                .conveyor_plane_z_mm = 800.0,
                .robot_base_x_mm = 50.0,
                .robot_base_y_mm = 25.0,
                .robot_base_z_mm = 0.0,
                .robot_base_yaw_deg = 0.0,
                .box_length_mm = 400.0,
                .box_width_mm = 200.0,
                .box_height_mm = 150.0,
                .coordinate_frame = "PI-GRIPPER-01_BASE",
                .calibration_version = 3,
            },
    });
    assert(orchestrator.BeginWork("MSG-BOX", kWorkId, "PI-INPUT-01").Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
    assert(orchestrator.ConfirmVisionAssignment("MSG-BOX", kWorkId).Applied());

    const auto position = Message("MSG-POSITION", mqtt::MessageType::kPositionDetected, "PI-VISION-01",
                                  mqtt::PositionDetectedPayload{
                                      .work_id = kWorkId,
                                      .box_x = 100,
                                      .box_y = 50,
                                      .box_width = 200,
                                      .box_height = 100,
                                      .center_x = 200,
                                      .center_y = 100,
                                      .offset_x = 0,
                                      .offset_y = 0,
                                      .position_status = "DETECTED",
                                      .box_corners =
                                          std::array{
                                              mqtt::PixelPoint{ .x = 100.0, .y = 50.0 },
                                              mqtt::PixelPoint{ .x = 300.0, .y = 50.0 },
                                              mqtt::PixelPoint{ .x = 300.0, .y = 150.0 },
                                              mqtt::PixelPoint{ .x = 100.0, .y = 150.0 },
                                          },
                                  });
    const auto position_result = orchestrator.Handle(position);
    assert(position_result.transition.Applied() && position_result.commands.size() == 1);
    const auto& input_stop = position_result.commands.front();
    const auto* input_stop_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(input_stop.message);
    assert(input_stop_payload != nullptr);
    assert(input_stop_payload->command == mqtt::ControlCommand::kStop);
    assert(input_stop_payload->target_device_id == "PI-INPUT-01");
    assert(input_stop_payload->component_id == "input_conveyor");
    assert(input_stop_payload->params.at("workId") == kWorkId);
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceCommandTopic("PI-INPUT-01"), input_stop.message).IsSuccess());
    assert(orchestrator.ConfirmDispatch(input_stop).Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kVisionProcessing);

    const auto barcode = Message("MSG-BARCODE", mqtt::MessageType::kBarcodeDetected, "PI-VISION-01",
                                 mqtt::BarcodeDetectedPayload{
                                     .work_id = kWorkId,
                                     .recognition_status = "SUCCESS",
                                     .barcode = "5901234123457",
                                     .confidence = 0.99,
                                     .message = std::nullopt,
                                 });
    assert(orchestrator.Preview(barcode).transition.Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kVisionProcessing);
    assert(orchestrator.Handle(barcode).transition.Applied());

    const auto product = Message("MSG-PRODUCT", mqtt::MessageType::kProductInfo, "central-server",
                                 mqtt::ProductInfoPayload{
                                     .work_id = kWorkId,
                                     .recognition_status = "SUCCESS",
                                     .barcode = "5901234123457",
                                     .product_id = "VEDA107",
                                     .product_name = "VEDA107",
                                     .destination = "1",
                                     .image = nullptr,
                                     .confidence = 0.99,
                                     .message = std::nullopt,
                                 });
    const auto product_result = orchestrator.Handle(product);
    assert(product_result.transition.Applied() && product_result.commands.size() == 2);
    const auto& line_tracer = product_result.commands.front();
    const auto* line_tracer_payload = mqtt::GetPayload<mqtt::DestinationSetPayload>(line_tracer.message);
    assert(line_tracer_payload != nullptr && line_tracer_payload->target_device_id == "PI-LT-01");
    assert(line_tracer_payload->work_id == kWorkId);
    assert(line_tracer_payload->destination == "1");
    assert(!line_tracer.dispatched_event.has_value());
    assert(orchestrator.ConfirmDispatch(line_tracer).Applied());
    const auto& gripper = product_result.commands.back();
    const auto* gripper_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(gripper.message);
    assert(gripper_payload != nullptr);
    assert(gripper_payload->command == mqtt::ControlCommand::kExecute);
    assert(gripper_payload->target_device_id == "PI-GRIPPER-01");
    assert(gripper_payload->params.at("workId") == kWorkId);
    assert(gripper_payload->params.at("action") == "PICK");
    assert(gripper_payload->params.at("coordinateFrame") == "PI-GRIPPER-01_BASE");
    assert(gripper_payload->params.at("unit") == "mm");
    assert(gripper_payload->params.at("targetPose").at("x") == 150.0);
    assert(gripper_payload->params.at("targetPose").at("y") == 75.0);
    assert(gripper_payload->params.at("targetPose").at("z") == 950.0);
    assert(gripper_payload->params.at("targetPose").at("yawDeg") == 0.0);
    assert(gripper_payload->params.at("calibrationVersion") == 3);
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceCommandTopic("PI-GRIPPER-01"), gripper.message).IsSuccess());
    assert(orchestrator.ConfirmDispatch(gripper).Applied());

    const auto line_tracer_ready = orchestrator.Handle(Status("MSG-LT-PICKUP", "PI-LT-01", "PICKUP_READY_A"));
    assert(!line_tracer_ready.handled);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);

    const auto unknown_gripper = orchestrator.Handle(Status("MSG-GRIPPER-FUTURE", "PI-GRIPPER-01", "FUTURE_STATE"));
    assert(!unknown_gripper.handled);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);
    const auto idle_gripper = orchestrator.Handle(Status("MSG-GRIPPER-READY", "PI-GRIPPER-01", "READY"));
    assert(!idle_gripper.handled);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);
    const auto early_completion = orchestrator.Handle(Status("MSG-GRIPPER-EARLY-DONE", "PI-GRIPPER-01", "COMPLETED"));
    assert(!early_completion.transition.Applied() && early_completion.commands.empty());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);
    assert(orchestrator.Handle(Status("MSG-GRIPPER-START", "PI-GRIPPER-01", "TRANSFERRING")).transition.Applied());
    const auto gripper_done = orchestrator.Handle(Status("MSG-GRIPPER-DONE", "PI-GRIPPER-01", "COMPLETED"));
    assert(gripper_done.transition.Applied() && gripper_done.commands.size() == 2);
    const auto& sorting = gripper_done.commands.front();
    const auto* sorting_payload = mqtt::GetPayload<mqtt::DestinationSetPayload>(sorting.message);
    assert(sorting_payload != nullptr && sorting_payload->target_device_id == "PI-SORTING-01");
    assert(orchestrator.ConfirmDispatch(sorting).Applied());
    const auto& input_start = gripper_done.commands.back();
    const auto* input_start_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(input_start.message);
    assert(input_start_payload != nullptr);
    assert(input_start_payload->command == mqtt::ControlCommand::kStart);
    assert(input_start_payload->target_device_id == "PI-INPUT-01");
    assert(input_start_payload->component_id == "input_conveyor");
    assert(input_start_payload->params.at("workId") == kWorkId);
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceCommandTopic("PI-INPUT-01"), input_start.message).IsSuccess());
    assert(orchestrator.ConfirmDispatch(input_start).Applied());

    assert(orchestrator.Handle(Status("MSG-SORTING-START", "PI-SORTING-01", "ROUTING")).transition.Applied());
    const auto sorting_done = orchestrator.Handle(Status("MSG-SORTING-DONE", "PI-SORTING-01", "CYCLE_COMPLETE"));
    assert(sorting_done.transition.Applied() && sorting_done.commands.empty());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kTransporting);

    assert(orchestrator.Handle(Status("MSG-LT-START", "PI-LT-01", "FOLLOWING_LINE")).transition.Applied());
    const auto completed = Message("MSG-COMPLETED", mqtt::MessageType::kWorkCompleted, "PI-LT-01",
                                   mqtt::WorkCompletedPayload{
                                       .work_id = kWorkId,
                                       .result = "SUCCESS",
                                       .message = std::string("unload complete"),
                                   });
    assert(orchestrator.Handle(completed).transition.Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kCompleted);
}

void TestInvalidOrderAndDispatchFailureEnterError() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-BOX-2", kWorkId, "PI-INPUT-01").Applied());
    assert(orchestrator.ConfirmVisionAssignment("MSG-BOX-2", kWorkId).Applied());
    const auto sorting = Status("MSG-SORT-EARLY", "PI-SORTING-01", "ROUTING");
    assert(orchestrator.Preview(sorting).transition.disposition == central_server::TransitionDisposition::kRejected);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kVisionAssigned);

    const auto barcode = Message("MSG-BARCODE-2", mqtt::MessageType::kBarcodeDetected, "PI-VISION-01",
                                 mqtt::BarcodeDetectedPayload{
                                     .work_id = kWorkId,
                                     .recognition_status = "SUCCESS",
                                     .barcode = "5901234123457",
                                     .confidence = std::nullopt,
                                     .message = std::nullopt,
                                 });
    assert(orchestrator.Handle(barcode).transition.Applied());
    const auto product = Message("MSG-PRODUCT-2", mqtt::MessageType::kProductInfo, "central-server",
                                 mqtt::ProductInfoPayload{
                                     .work_id = kWorkId,
                                     .recognition_status = "SUCCESS",
                                     .barcode = "5901234123457",
                                     .product_id = "VEDA107",
                                     .product_name = "VEDA107",
                                     .destination = "1",
                                     .image = nullptr,
                                     .confidence = std::nullopt,
                                     .message = std::nullopt,
                                 });
    const auto product_result = orchestrator.Handle(product);
    assert(product_result.commands.size() == 2);
    assert(orchestrator.FailDispatch(product_result.commands.back(), "gripper is offline").Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);
}

void TestInputFailureWithoutWorkIdStopsTheProcess() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-BOX-3", kWorkId, "PI-INPUT-01").Applied());
    const auto input_error = Message("MSG-INPUT-ERROR", mqtt::MessageType::kErrorOccurred, "PI-INPUT-01",
                                     mqtt::ErrorOccurredPayload{
                                         .job_id = std::nullopt,
                                         .error_code = "INPUT_JAM",
                                         .error_level = "ERROR",
                                         .current_state = "FAULT",
                                         .message = "input conveyor jammed",
                                         .distance = std::nullopt,
                                     });
    const auto result = orchestrator.Handle(input_error);
    assert(result.handled);
    assert(result.transition.Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kFailed);
}

void TestInputOfflineStatusStopsTheProcess() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-BOX-4", kWorkId, "PI-INPUT-01").Applied());
    const auto offline = Message("MSG-INPUT-OFFLINE", mqtt::MessageType::kDeviceStatus, "PI-INPUT-01",
                                 mqtt::DeviceStatusPayload{
                                     .status = mqtt::ConnectionState::kOffline,
                                     .current_state = "FAULT",
                                     .job_id = std::nullopt,
                                     .error_code = std::string("INPUT_OFFLINE"),
                                 });
    assert(orchestrator.Handle(offline).transition.Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);
}

void TestEveryConfiguredNodeFailureStopsAndRecoversTheProcess() {
    struct FailureCase {
        std::string_view device_id;
        mqtt::ConnectionState connection;
    };
    constexpr std::array failures{
        FailureCase{ "PI-INPUT-01", mqtt::ConnectionState::kOffline },
        FailureCase{ "PI-VISION-01", mqtt::ConnectionState::kRtspError },
        FailureCase{ "PI-GRIPPER-01", mqtt::ConnectionState::kMqttError },
        FailureCase{ "PI-SORTING-01", mqtt::ConnectionState::kUartError },
        FailureCase{ "PI-LT-01", mqtt::ConnectionState::kOffline },
    };

    for (std::size_t index = 0; index < failures.size(); ++index) {
        central_server::ProcessOrchestrator orchestrator({ .enabled = true });
        assert(orchestrator.BeginWork("MSG-NODE-WORK-" + std::to_string(index), kWorkId, "PI-INPUT-01").Applied());
        const auto result = orchestrator.Handle(Status("MSG-NODE-FAILURE-" + std::to_string(index),
                                                       std::string(failures[index].device_id), "STOPPED",
                                                       failures[index].connection, std::nullopt));
        assert(result.handled && result.transition.Applied());
        assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);
        const auto failed = orchestrator.StateMachine().FindWork(kWorkId);
        assert(failed->stage == central_server::WorkStage::kFailed);
        assert(failed->suspended_stage == central_server::WorkStage::kInputDetected);

        assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
        if (failures[index].device_id == "PI-LT-01") {
            const auto reset = orchestrator.Handle(Status("MSG-LINE-RESET", "PI-LT-01", "POSITION_UNKNOWN",
                                                          mqtt::ConnectionState::kOnline, std::nullopt, true));
            assert(!reset.handled);
            assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRecovery);
        }
        assert(orchestrator.CompleteSystemRecovery().Applied());
        assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());
        assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
    }
}

void TestHealthyStoppedNodesDoNotFailTheProcess() {
    constexpr std::array device_ids{
        "PI-INPUT-01", "PI-VISION-01", "PI-GRIPPER-01", "PI-SORTING-01", "PI-LT-01",
    };
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-HEALTHY-IDLE-WORK", kWorkId, "PI-INPUT-01").Applied());

    for (std::size_t index = 0; index < device_ids.size(); ++index) {
        const auto result = orchestrator.Handle(Status("MSG-HEALTHY-IDLE-" + std::to_string(index), device_ids[index],
                                                       "STOPPED", mqtt::ConnectionState::kOnline, std::nullopt));
        assert(!result.handled);
        assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
        assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
    }
}

void TestIdleSystemRecoversAfterEveryNodeReportsHealthy() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    const auto failure = orchestrator.Handle(
        Status("MSG-AUTO-RECOVERY-FAILURE", "PI-INPUT-01", "FAULT", mqtt::ConnectionState::kOffline, std::nullopt));
    assert(failure.handled && failure.transition.Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);

    assert(!orchestrator.Handle(Heartbeat("MSG-AUTO-RECOVERY-INPUT", "PI-INPUT-01", "STOPPED")).handled);
    assert(!orchestrator.Handle(Heartbeat("MSG-AUTO-RECOVERY-VISION", "PI-VISION-01", "STOPPED")).handled);
    assert(!orchestrator.Handle(Heartbeat("MSG-AUTO-RECOVERY-GRIPPER", "PI-GRIPPER-01", "STOPPED")).handled);
    assert(!orchestrator.Handle(Heartbeat("MSG-AUTO-RECOVERY-SORTING", "PI-SORTING-01", "STOPPED")).handled);

    const auto unknown_position =
        orchestrator.Handle(Heartbeat("MSG-AUTO-RECOVERY-LT-UNKNOWN", "PI-LT-01", "POSITION_UNKNOWN"));
    assert(!unknown_position.handled);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);

    const auto recovered = orchestrator.Handle(Heartbeat("MSG-AUTO-RECOVERY-LT-READY", "PI-LT-01", "IDLE"));
    assert(recovered.handled && recovered.transition.Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kIdle);
    assert(orchestrator.BeginWork("MSG-AUTO-RECOVERY-WORK", kWorkId, "PI-INPUT-01").Applied());
}

void TestActiveWorkPreventsAutomaticRecovery() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-ACTIVE-RECOVERY-WORK", kWorkId, "PI-INPUT-01").Applied());
    assert(orchestrator
               .Handle(Status("MSG-ACTIVE-RECOVERY-FAILURE", "PI-GRIPPER-01", "FAULT", mqtt::ConnectionState::kOffline,
                              std::nullopt))
               .transition.Applied());

    constexpr std::array healthy_nodes{
        std::pair{ "PI-INPUT-01", "RUNNING" },   std::pair{ "PI-VISION-01", "WAITING_FOR_PRODUCT" },
        std::pair{ "PI-GRIPPER-01", "RUNNING" }, std::pair{ "PI-SORTING-01", "RUNNING" },
        std::pair{ "PI-LT-01", "IDLE" },
    };
    for (std::size_t index = 0; index < healthy_nodes.size(); ++index) {
        static_cast<void>(orchestrator.Handle(Status("MSG-ACTIVE-RECOVERY-HEALTHY-" + std::to_string(index),
                                                     healthy_nodes[index].first, healthy_nodes[index].second,
                                                     mqtt::ConnectionState::kOnline, std::nullopt)));
    }
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kFailed);
}

void TestIdleEmergencyStopRecoversAfterEveryNodeReportsHealthy() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    const auto emergency_stop = orchestrator.Handle(
        Status("MSG-ESTOP-INPUT", "PI-INPUT-01", "EMERGENCY_STOP", mqtt::ConnectionState::kOnline, std::nullopt));
    assert(emergency_stop.handled && emergency_stop.transition.Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kEmergencyStop);

    assert(!orchestrator.Handle(Heartbeat("MSG-ESTOP-VISION", "PI-VISION-01", "STOPPED")).handled);
    assert(!orchestrator.Handle(Heartbeat("MSG-ESTOP-GRIPPER", "PI-GRIPPER-01", "STOPPED")).handled);
    assert(!orchestrator.Handle(Heartbeat("MSG-ESTOP-SORTING", "PI-SORTING-01", "STOPPED")).handled);
    assert(!orchestrator.Handle(Heartbeat("MSG-ESTOP-LT", "PI-LT-01", "IDLE")).handled);

    const auto recovered = orchestrator.Handle(
        Status("MSG-ESTOP-INPUT-RECOVERED", "PI-INPUT-01", "STOPPED", mqtt::ConnectionState::kOnline, std::nullopt));
    assert(recovered.handled && recovered.transition.Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kStopped);
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
}

void TestFailedWorkIsDiscardedAfterServerRestart() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    std::vector works{
        central_server::WorkProcessSnapshot{
            .work_id = kWorkId,
            .stage = central_server::WorkStage::kFailed,
            .suspended_stage = central_server::WorkStage::kInputDetected,
            .destination = "1",
            .last_source_id = "PI-INPUT-01",
            .failure_reason = "input conveyor fault",
        },
    };

    const auto restore =
        orchestrator.RestoreAfterServerRestart(central_server::ProcessSystemState::kError, std::move(works), {}, 12);
    assert(restore.restored);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kError);
    assert(orchestrator.StateMachine().ActiveWorks().empty());
    assert(!orchestrator.StateMachine().FindWork(kWorkId).has_value());
}

void TestDeviceEmergencyStopPreservesEmergencyState() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-ESTOP-WORK", kWorkId, "PI-INPUT-01").Applied());

    const auto spontaneous = orchestrator.Handle(
        Status("MSG-ESTOP-INPUT", "PI-INPUT-01", "EMERGENCY_STOP", mqtt::ConnectionState::kOnline, std::nullopt));
    assert(spontaneous.handled && spontaneous.transition.Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kEmergencyStop);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kEmergencyStopped);

    const auto confirmation = orchestrator.Handle(
        Status("MSG-ESTOP-LINE", "PI-LT-01", "EMERGENCY_STOP", mqtt::ConnectionState::kOnline, std::nullopt));
    assert(confirmation.handled);
    assert(confirmation.transition.disposition == central_server::TransitionDisposition::kDuplicate);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kEmergencyStop);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kEmergencyStopped);
}

void TestRestoredHomographyTargetCreatesGripperCommand() {
    central_server::ProcessOrchestrator orchestrator({
        .enabled = true,
        .server_id = "central-server",
        .input_device_id = "PI-INPUT-01",
        .vision_device_id = "PI-VISION-01",
        .gripper_device_id = "PI-GRIPPER-01",
        .sorting_device_id = "PI-SORTING-01",
        .line_tracer_device_id = "PI-LT-01",
        .line_tracer_initial_position = {},
        .homography =
            {
                .enabled = true,
                .pixel_to_conveyor = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 },
                .conveyor_plane_z_mm = 800.0,
                .robot_base_x_mm = 50.0,
                .robot_base_y_mm = 25.0,
                .robot_base_z_mm = 0.0,
                .robot_base_yaw_deg = 0.0,
                .box_length_mm = 400.0,
                .box_width_mm = 200.0,
                .box_height_mm = 150.0,
                .coordinate_frame = "PI-GRIPPER-01_BASE",
                .calibration_version = 3,
            },
    });
    std::vector works{
        central_server::WorkProcessSnapshot{
            .work_id = kWorkId,
            .stage = central_server::WorkStage::kBarcodeRecognized,
            .suspended_stage = std::nullopt,
            .destination = {},
            .last_source_id = "PI-VISION-01",
            .failure_reason = {},
        },
    };
    std::unordered_map<std::string, central_server::GripperTarget> targets{
        { kWorkId,
          {
              .x_mm = 150.0,
              .y_mm = 75.0,
              .z_mm = 950.0,
              .yaw_deg = 10.0,
              .box_length_mm = 400.0,
              .box_width_mm = 200.0,
              .box_height_mm = 150.0,
              .coordinate_frame = "PI-GRIPPER-01_BASE",
              .calibration_version = 3,
          } },
    };
    const auto restore = orchestrator.RestoreAfterServerRestart(central_server::ProcessSystemState::kRunning,
                                                                std::move(works), std::move(targets), 12);
    assert(restore.restored);
    assert(restore.invalidated_works.empty());
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());

    const auto failure = Message("MSG-RECOVERABLE-FAILURE", mqtt::MessageType::kErrorOccurred, "PI-VISION-01",
                                 mqtt::ErrorOccurredPayload{
                                     .job_id = std::string(kWorkId),
                                     .error_code = "BARCODE_RETRY",
                                     .error_level = "ERROR",
                                     .current_state = "FAULT",
                                     .message = "barcode processing will be retried",
                                     .distance = std::nullopt,
                                 });
    assert(orchestrator.Handle(failure).transition.Applied());
    assert(orchestrator.GripperTargets().contains(kWorkId));
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(orchestrator.CompleteSystemRecovery().Applied());
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());

    const auto product = Message("MSG-PRODUCT-RESTORED", mqtt::MessageType::kProductInfo, "central-server",
                                 mqtt::ProductInfoPayload{
                                     .work_id = kWorkId,
                                     .recognition_status = "SUCCESS",
                                     .barcode = "5901234123457",
                                     .product_id = "VEDA107",
                                     .product_name = "VEDA107",
                                     .destination = "1",
                                     .image = nullptr,
                                     .confidence = 0.99,
                                     .message = std::nullopt,
                                 });
    const auto result = orchestrator.Handle(product);
    assert(result.transition.Applied());
    assert(result.commands.size() == 2);
    const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(result.commands.back().message);
    assert(command != nullptr);
    assert(command->params.at("targetPose").at("x") == 150.0);
    assert(command->params.at("targetPose").at("yawDeg") == 10.0);
}

void TestDisabledHomographyDiscardsRestoredTarget() {
    central_server::ProcessOrchestrator orchestrator({
        .enabled = true,
        .server_id = "central-server",
        .input_device_id = "PI-INPUT-01",
        .vision_device_id = "PI-VISION-01",
        .gripper_device_id = "PI-GRIPPER-01",
        .sorting_device_id = "PI-SORTING-01",
        .line_tracer_device_id = "PI-LT-01",
        .line_tracer_initial_position = {},
        .homography = { .enabled = false },
    });
    std::vector works{
        central_server::WorkProcessSnapshot{
            .work_id = kWorkId,
            .stage = central_server::WorkStage::kBarcodeRecognized,
            .suspended_stage = std::nullopt,
            .destination = {},
            .last_source_id = "PI-VISION-01",
            .failure_reason = {},
        },
    };
    std::unordered_map<std::string, central_server::GripperTarget> targets{
        { kWorkId,
          {
              .x_mm = 150.0,
              .y_mm = 75.0,
              .z_mm = 950.0,
              .yaw_deg = 10.0,
              .box_length_mm = 400.0,
              .box_width_mm = 200.0,
              .box_height_mm = 150.0,
              .coordinate_frame = "PI-GRIPPER-01_BASE",
              .calibration_version = 3,
          } },
    };
    const auto restore = orchestrator.RestoreAfterServerRestart(central_server::ProcessSystemState::kRunning,
                                                                std::move(works), std::move(targets), 12);
    assert(restore.restored);
    assert(restore.invalidated_works.empty());
    assert(orchestrator.GripperTargets().empty());
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());

    const auto product = Message("MSG-PRODUCT-NO-HOMOGRAPHY", mqtt::MessageType::kProductInfo, "central-server",
                                 mqtt::ProductInfoPayload{
                                     .work_id = kWorkId,
                                     .recognition_status = "SUCCESS",
                                     .barcode = "5901234123457",
                                     .product_id = "VEDA107",
                                     .product_name = "VEDA107",
                                     .destination = "1",
                                     .image = nullptr,
                                     .confidence = 0.99,
                                     .message = std::nullopt,
                                 });
    const auto result = orchestrator.Handle(product);
    assert(result.transition.Applied());
    assert(result.commands.size() == 2);
    const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(result.commands.back().message);
    assert(command != nullptr);
    assert(!command->params.contains("targetPose"));
}

void TestChangedCalibrationDiscardsRestoredTarget() {
    central_server::ProcessOrchestrator orchestrator({
        .enabled = true,
        .server_id = "central-server",
        .input_device_id = "PI-INPUT-01",
        .vision_device_id = "PI-VISION-01",
        .gripper_device_id = "PI-GRIPPER-01",
        .sorting_device_id = "PI-SORTING-01",
        .line_tracer_device_id = "PI-LT-01",
        .line_tracer_initial_position = {},
        .homography =
            {
                .enabled = true,
                .pixel_to_conveyor = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 },
                .conveyor_plane_z_mm = 800.0,
                .robot_base_x_mm = 50.0,
                .robot_base_y_mm = 25.0,
                .robot_base_z_mm = 0.0,
                .robot_base_yaw_deg = 0.0,
                .box_length_mm = 400.0,
                .box_width_mm = 200.0,
                .box_height_mm = 150.0,
                .coordinate_frame = "PI-GRIPPER-01_BASE",
                .calibration_version = 4,
            },
    });
    std::vector works{
        central_server::WorkProcessSnapshot{
            .work_id = kWorkId,
            .stage = central_server::WorkStage::kBarcodeRecognized,
            .suspended_stage = std::nullopt,
            .destination = {},
            .last_source_id = "PI-VISION-01",
            .failure_reason = {},
        },
    };
    std::unordered_map<std::string, central_server::GripperTarget> targets{
        { kWorkId,
          {
              .x_mm = 150.0,
              .y_mm = 75.0,
              .z_mm = 950.0,
              .yaw_deg = 10.0,
              .box_length_mm = 400.0,
              .box_width_mm = 200.0,
              .box_height_mm = 150.0,
              .coordinate_frame = "PI-GRIPPER-01_BASE",
              .calibration_version = 3,
          } },
    };
    const auto restore = orchestrator.RestoreAfterServerRestart(central_server::ProcessSystemState::kRunning,
                                                                std::move(works), std::move(targets), 12);
    assert(restore.restored);
    assert(restore.invalidated_works.size() == 1);
    assert(restore.invalidated_works.front().work_id == kWorkId);
    assert(orchestrator.GripperTargets().empty());
    assert(!orchestrator.StateMachine().FindWork(kWorkId).has_value());
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());

    const auto product = Message("MSG-PRODUCT-STALE-CALIBRATION", mqtt::MessageType::kProductInfo, "central-server",
                                 mqtt::ProductInfoPayload{
                                     .work_id = kWorkId,
                                     .recognition_status = "SUCCESS",
                                     .barcode = "5901234123457",
                                     .product_id = "VEDA107",
                                     .product_name = "VEDA107",
                                     .destination = "1",
                                     .image = nullptr,
                                     .confidence = 0.99,
                                     .message = std::nullopt,
                                 });
    const auto result = orchestrator.Handle(product);
    assert(result.transition.disposition == central_server::TransitionDisposition::kRejected);
    assert(result.commands.empty());

    assert(orchestrator.BeginWork("MSG-REPLACEMENT-BOX", kReplacementWorkId, "PI-INPUT-01").Applied());
    assert(orchestrator.ConfirmVisionAssignment("MSG-REPLACEMENT-BOX", kReplacementWorkId).Applied());
    const auto replacement_position =
        Message("MSG-REPLACEMENT-POSITION", mqtt::MessageType::kPositionDetected, "PI-VISION-01",
                mqtt::PositionDetectedPayload{
                    .work_id = kReplacementWorkId,
                    .box_x = 100,
                    .box_y = 50,
                    .box_width = 200,
                    .box_height = 100,
                    .center_x = 200,
                    .center_y = 100,
                    .offset_x = 0,
                    .offset_y = 0,
                    .position_status = "DETECTED",
                    .box_corners =
                        std::array{
                            mqtt::PixelPoint{ .x = 100.0, .y = 50.0 },
                            mqtt::PixelPoint{ .x = 300.0, .y = 50.0 },
                            mqtt::PixelPoint{ .x = 300.0, .y = 150.0 },
                            mqtt::PixelPoint{ .x = 100.0, .y = 150.0 },
                        },
                });
    assert(orchestrator.Handle(replacement_position).transition.Applied());
    assert(orchestrator.GripperTargets().contains(kReplacementWorkId));
}

void TestDownstreamDevicesServeOneWorkAtATime() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    const auto route_work = [&orchestrator](std::string_view work_id, std::string_view suffix) {
        const std::string id_suffix(suffix);
        assert(orchestrator.BeginWork("MSG-BOX-" + id_suffix, work_id, "PI-INPUT-01").Applied());
        assert(orchestrator.ConfirmVisionAssignment("MSG-BOX-" + id_suffix, work_id).Applied());
        assert(orchestrator
                   .Handle(Message("MSG-BARCODE-" + id_suffix, mqtt::MessageType::kBarcodeDetected, "PI-VISION-01",
                                   mqtt::BarcodeDetectedPayload{
                                       .work_id = std::string(work_id),
                                       .recognition_status = "SUCCESS",
                                       .barcode = "5901234123457",
                                       .confidence = std::nullopt,
                                       .message = std::nullopt,
                                   }))
                   .transition.Applied());
        return orchestrator.Handle(Message("MSG-PRODUCT-" + id_suffix, mqtt::MessageType::kProductInfo,
                                           "central-server",
                                           mqtt::ProductInfoPayload{
                                               .work_id = std::string(work_id),
                                               .recognition_status = "SUCCESS",
                                               .barcode = "5901234123457",
                                               .product_id = "VEDA107",
                                               .product_name = "VEDA107",
                                               .destination = "1",
                                               .image = nullptr,
                                               .confidence = std::nullopt,
                                               .message = std::nullopt,
                                           }));
    };

    const auto first = route_work(kWorkId, "FIRST");
    assert(first.commands.size() == 2);
    for (const auto& command : first.commands) {
        assert(orchestrator.ConfirmDispatch(command).Applied());
    }

    const auto queued = route_work(kQueuedWorkId, "QUEUED");
    assert(queued.transition.Applied());
    assert(queued.commands.empty());
    assert(orchestrator.StateMachine().FindWork(kQueuedWorkId)->stage == central_server::WorkStage::kProductIdentified);

    assert(orchestrator
               .Handle(Status("MSG-GRIPPER-FIRST-START", "PI-GRIPPER-01", "TRANSFERRING",
                              mqtt::ConnectionState::kOnline, std::string(kWorkId)))
               .transition.Applied());
    const auto gripper_done = orchestrator.Handle(Status("MSG-GRIPPER-FIRST-DONE", "PI-GRIPPER-01", "COMPLETED",
                                                         mqtt::ConnectionState::kOnline, std::string(kWorkId)));
    assert(gripper_done.commands.size() == 2);
    for (const auto& command : gripper_done.commands) {
        assert(orchestrator.ConfirmDispatch(command).Applied());
    }
    assert(orchestrator
               .Handle(Status("MSG-SORTING-FIRST-START", "PI-SORTING-01", "ROUTING", mqtt::ConnectionState::kOnline,
                              std::string(kWorkId)))
               .transition.Applied());
    assert(orchestrator
               .Handle(Status("MSG-SORTING-FIRST-DONE", "PI-SORTING-01", "CYCLE_COMPLETE",
                              mqtt::ConnectionState::kOnline, std::string(kWorkId)))
               .transition.Applied());
    assert(orchestrator
               .Handle(Status("MSG-LT-FIRST-START", "PI-LT-01", "FOLLOWING_LINE", mqtt::ConnectionState::kOnline,
                              std::string(kWorkId)))
               .transition.Applied());

    const auto completed =
        orchestrator.Handle(Message("MSG-FIRST-COMPLETED", mqtt::MessageType::kWorkCompleted, "PI-LT-01",
                                    mqtt::WorkCompletedPayload{
                                        .work_id = kWorkId,
                                        .result = "SUCCESS",
                                        .message = std::string("unload complete"),
                                    }));
    assert(completed.transition.Applied());
    assert(completed.commands.size() == 2);
    assert(completed.commands.front().work_id == kQueuedWorkId);
    assert(completed.commands.back().work_id == kQueuedWorkId);
    const auto* line_tracer = mqtt::GetPayload<mqtt::DestinationSetPayload>(completed.commands.front().message);
    const auto* gripper = mqtt::GetPayload<mqtt::ControlCommandPayload>(completed.commands.back().message);
    assert(line_tracer != nullptr && line_tracer->target_device_id == "PI-LT-01");
    assert(gripper != nullptr && gripper->target_device_id == "PI-GRIPPER-01");
}

}  // namespace

int main() {
    TestEventFlowCreatesCommandsForEachNode();
    TestInvalidOrderAndDispatchFailureEnterError();
    TestInputFailureWithoutWorkIdStopsTheProcess();
    TestInputOfflineStatusStopsTheProcess();
    TestEveryConfiguredNodeFailureStopsAndRecoversTheProcess();
    TestHealthyStoppedNodesDoNotFailTheProcess();
    TestIdleSystemRecoversAfterEveryNodeReportsHealthy();
    TestActiveWorkPreventsAutomaticRecovery();
    TestIdleEmergencyStopRecoversAfterEveryNodeReportsHealthy();
    TestFailedWorkIsDiscardedAfterServerRestart();
    TestDeviceEmergencyStopPreservesEmergencyState();
    TestRestoredHomographyTargetCreatesGripperCommand();
    TestDisabledHomographyDiscardsRestoredTarget();
    TestChangedCalibrationDiscardsRestoredTarget();
    TestDownstreamDevicesServeOneWorkAtATime();
    return 0;
}
