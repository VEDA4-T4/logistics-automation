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

mqtt::MqttMessage SuccessResponse(std::string id, std::string source,
                                  const central_server::ProcessCommandIntent& intent) {
    std::string request_id;
    mqtt::ControlCommand command{ mqtt::ControlCommand::kUnknown };
    if (const auto* control = mqtt::GetPayload<mqtt::ControlCommandPayload>(intent.message)) {
        request_id = control->request_id;
        command = control->command;
    } else if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(intent.message)) {
        request_id = destination->request_id;
        command = destination->command;
    }
    assert(!request_id.empty() && command != mqtt::ControlCommand::kUnknown);
    return Message(std::move(id), mqtt::MessageType::kCommandResponse, std::move(source),
                   mqtt::CommandResponsePayload{
                       .request_id = std::move(request_id),
                       .command = command,
                       .result = mqtt::CommandResult::kSuccess,
                       .error_code = std::nullopt,
                       .message = "command completed",
                   });
}

void TestOnlyInputNodeCanCreateWork() {
    central_server::ProcessOrchestrator orchestrator({
        .input_device_id = "PI-INPUT-01",
        .vision_device_id = "PI-VISION-01",
    });
    assert(orchestrator.IsWorkCreationSource("PI-INPUT-01"));
    assert(!orchestrator.IsWorkCreationSource("PI-VISION-01"));
    assert(!orchestrator.IsWorkCreationSource("PI-SORTING-01"));
}

void TestProcessCommandIdsAreScopedToProcessEpoch() {
    central_server::ProcessOrchestrator first;
    first.SetProcessEpoch("11111111-1111-4111-8111-111111111111");
    const auto first_begin = first.BeginWork("MSG-FIRST", kWorkId, "PI-INPUT-01", kTimestamp);
    assert(first_begin.transition.Applied() && first_begin.commands.size() == 1);
    const auto* first_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(first_begin.commands.front().message);
    assert(first_payload != nullptr);
    assert(first_payload->request_id == first_begin.commands.front().message.message_id);
    assert(first_begin.commands.front().message.message_id == "PROCESS-11111111-1111-4111-8111-111111111111-1");

    central_server::ProcessOrchestrator second;
    second.SetProcessEpoch("22222222-2222-4222-8222-222222222222");
    const auto second_begin = second.BeginWork("MSG-SECOND", kQueuedWorkId, "PI-INPUT-01", kTimestamp);
    assert(second_begin.transition.Applied() && second_begin.commands.size() == 1);
    assert(second_begin.commands.front().message.message_id == "PROCESS-22222222-2222-4222-8222-222222222222-1");
    assert(first_begin.commands.front().message.message_id != second_begin.commands.front().message.message_id);
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
    const auto begin = orchestrator.BeginWork("MSG-BOX", kWorkId, "PI-INPUT-01", kTimestamp);
    assert(begin.transition.Applied() && begin.commands.size() == 1);
    const auto& input_stop = begin.commands.front();
    const auto* input_stop_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(input_stop.message);
    assert(input_stop_payload != nullptr);
    assert(input_stop_payload->command == mqtt::ControlCommand::kStop);
    assert(input_stop_payload->target_device_id == "PI-INPUT-01");
    assert(input_stop_payload->component_id == "input_conveyor");
    assert(input_stop_payload->params.at("workId") == kWorkId);
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceCommandTopic("PI-INPUT-01"), input_stop.message).IsSuccess());
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
    assert(position_result.transition.Applied() && position_result.commands.empty());
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
    assert(product_result.transition.Applied() && product_result.commands.size() == 1);
    const auto& line_tracer = product_result.commands.front();
    const auto* line_tracer_payload = mqtt::GetPayload<mqtt::DestinationSetPayload>(line_tracer.message);
    assert(line_tracer_payload != nullptr && line_tracer_payload->target_device_id == "PI-LT-01");
    assert(line_tracer_payload->work_id == kWorkId);
    assert(line_tracer_payload->destination == "1");
    assert(!line_tracer.dispatched_event.has_value());
    assert(orchestrator.ConfirmDispatch(line_tracer).Applied());

    const auto line_tracer_ready = orchestrator.Handle(
        Status("MSG-LT-PICKUP", "PI-LT-01", "PICKUP_READY_A", mqtt::ConnectionState::kOnline, std::nullopt));
    assert(line_tracer_ready.handled && line_tracer_ready.commands.size() == 1);
    const auto& gripper = line_tracer_ready.commands.front();
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
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);
    const auto duplicate_pickup = orchestrator.Handle(
        Status("MSG-LT-PICKUP-DUPLICATE", "PI-LT-01", "PICKUP_READY_A", mqtt::ConnectionState::kOnline, std::nullopt));
    assert(!duplicate_pickup.handled && duplicate_pickup.commands.empty());

    const auto unknown_gripper = orchestrator.Handle(Status("MSG-GRIPPER-FUTURE", "PI-GRIPPER-01", "FUTURE_STATE"));
    assert(!unknown_gripper.handled);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);
    const auto idle_gripper = orchestrator.Handle(Status("MSG-GRIPPER-READY", "PI-GRIPPER-01", "READY"));
    assert(!idle_gripper.handled);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);
    const auto early_completion = orchestrator.Handle(Status("MSG-GRIPPER-EARLY-DONE", "PI-GRIPPER-01", "COMPLETED"));
    assert(!early_completion.transition.Applied() && early_completion.commands.empty());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kGripperRequested);
    const auto gripper_done = orchestrator.HandleCommandCompletion(
        gripper, Message("MSG-GRIPPER-DONE", mqtt::MessageType::kCommandResponse, "PI-GRIPPER-01",
                         mqtt::CommandResponsePayload{
                             .request_id = gripper_payload->request_id,
                             .command = mqtt::ControlCommand::kExecute,
                             .result = mqtt::CommandResult::kSuccess,
                             .error_code = std::nullopt,
                             .message = "gripper transfer completed",
                         }));
    assert(gripper_done.transition.Applied() && gripper_done.commands.size() == 1);
    const auto& sorting = gripper_done.commands.front();
    const auto* sorting_payload = mqtt::GetPayload<mqtt::DestinationSetPayload>(sorting.message);
    assert(sorting_payload != nullptr && sorting_payload->target_device_id == "PI-SORTING-01");
    const auto premature_before_destination =
        orchestrator.Handle(Status("MSG-SORTING-PREMATURE-BEFORE-DESTINATION", "PI-SORTING-01", "CYCLE_COMPLETE"));
    assert(!premature_before_destination.transition.Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kSortingRequested);
    assert(orchestrator.ConfirmDispatch(sorting).Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kSortingRequested);

    const auto revision_after_terminal_response = orchestrator.Revision();
    const auto duplicate_status =
        orchestrator.Handle(Status("MSG-GRIPPER-DUPLICATE-DONE", "PI-GRIPPER-01", "COMPLETED"));
    assert(!duplicate_status.transition.Applied() && duplicate_status.commands.empty());
    assert(orchestrator.Revision() == revision_after_terminal_response);

    const auto sorting_destination_done = orchestrator.HandleCommandCompletion(
        sorting, Message("MSG-SORTING-DESTINATION-DONE", mqtt::MessageType::kCommandResponse, "PI-SORTING-01",
                         mqtt::CommandResponsePayload{
                             .request_id = sorting_payload->request_id,
                             .command = mqtt::ControlCommand::kDestinationSet,
                             .result = mqtt::CommandResult::kSuccess,
                             .error_code = std::nullopt,
                             .message = "sorting destination accepted",
                         }));
    assert(sorting_destination_done.commands.size() == 1);
    const auto& sorting_start = sorting_destination_done.commands.front();
    const auto* sorting_start_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(sorting_start.message);
    assert(sorting_start_payload != nullptr);
    assert(sorting_start_payload->command == mqtt::ControlCommand::kStart);
    assert(sorting_start_payload->target_device_id == "PI-SORTING-01");
    assert(sorting_start_payload->component_id == "sorting_conveyor");
    const auto premature_before_start =
        orchestrator.Handle(Status("MSG-SORTING-PREMATURE-BEFORE-START", "PI-SORTING-01", "CYCLE_COMPLETE"));
    assert(!premature_before_start.transition.Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kSortingRequested);
    assert(orchestrator.ConfirmDispatch(sorting_start).Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kSortingRequested);

    const auto sorting_start_done = orchestrator.HandleCommandCompletion(
        sorting_start, Message("MSG-SORTING-START-DONE", mqtt::MessageType::kCommandResponse, "PI-SORTING-01",
                               mqtt::CommandResponsePayload{
                                   .request_id = sorting_start_payload->request_id,
                                   .command = mqtt::ControlCommand::kStart,
                                   .result = mqtt::CommandResult::kSuccess,
                                   .error_code = std::nullopt,
                                   .message = "sorting conveyor started",
                               }));
    assert(sorting_start_done.transition.Applied() && sorting_start_done.commands.empty());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kSorting);

    assert(orchestrator.Handle(Status("MSG-SORTING-START", "PI-SORTING-01", "ROUTING")).transition.Applied());
    const auto sorting_detection = orchestrator.SortingDetectionCommands(kWorkId, kTimestamp);
    assert(sorting_detection.size() == 1);
    const auto* sorting_stop = mqtt::GetPayload<mqtt::ControlCommandPayload>(sorting_detection.front().message);
    assert(sorting_stop != nullptr);
    assert(sorting_stop->command == mqtt::ControlCommand::kStop);
    assert(sorting_stop->target_device_id == "PI-SORTING-01");
    assert(sorting_stop->component_id == "sorting_conveyor");
    assert(sorting_stop->params.at("workId") == kWorkId);
    const auto gate_recovery = orchestrator.HandleCommandCompletion(
        sorting_detection.front(),
        Message("MSG-SORTING-STOP-DONE", mqtt::MessageType::kCommandResponse, "PI-SORTING-01",
                mqtt::CommandResponsePayload{
                    .request_id = sorting_stop->request_id,
                    .command = mqtt::ControlCommand::kStop,
                    .result = mqtt::CommandResult::kSuccess,
                    .error_code = std::nullopt,
                    .message = "sorting conveyor stopped",
                }));
    assert(gate_recovery.commands.size() == 1);
    const auto* return_home = mqtt::GetPayload<mqtt::ControlCommandPayload>(gate_recovery.commands.front().message);
    assert(return_home != nullptr);
    assert(return_home->command == mqtt::ControlCommand::kRecovery);
    assert(return_home->target_device_id == "PI-SORTING-01");
    assert(return_home->component_id == "GATE");
    assert(return_home->params.at("workId") == kWorkId);
    assert(orchestrator.SortingDetectionCommands("UNKNOWN-WORK", kTimestamp).empty());
    const auto sorting_done = orchestrator.Handle(Status("MSG-SORTING-DONE", "PI-SORTING-01", "CYCLE_COMPLETE"));
    assert(sorting_done.transition.Applied() && sorting_done.commands.empty());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kTransporting);

    assert(!orchestrator.Handle(Status("MSG-LT-START", "PI-LT-01", "FOLLOWING_LINE")).handled);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kTransporting);
    const auto completed = Message("MSG-COMPLETED", mqtt::MessageType::kWorkCompleted, "PI-LT-01",
                                   mqtt::WorkCompletedPayload{
                                       .work_id = kWorkId,
                                       .result = "SUCCESS",
                                       .message = std::string("unload complete"),
                                   });
    const auto unload_completed = orchestrator.Handle(completed);
    assert(unload_completed.transition.Applied());
    assert(unload_completed.commands.size() == 1);
    const auto* input_start = mqtt::GetPayload<mqtt::ControlCommandPayload>(unload_completed.commands.front().message);
    assert(input_start != nullptr);
    assert(input_start->command == mqtt::ControlCommand::kStart);
    assert(input_start->target_device_id == "PI-INPUT-01");
    assert(input_start->component_id == "input_conveyor");
    assert(input_start->params.at("workId") == kWorkId);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kCompleted);
}

void TestInputDetectionSafetyStopDoesNotCreateWork() {
    central_server::ProcessOrchestrator orchestrator;
    const auto stop = orchestrator.MakeInputConveyorSafetyStop("MSG-SENSOR-DETECTED", kTimestamp);
    const auto* payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(stop);
    assert(payload != nullptr);
    assert(stop.message_id == "INPUT-DETECTION-STOP-MSG-SENSOR-DETECTED");
    assert(payload->request_id == stop.message_id);
    assert(payload->command == mqtt::ControlCommand::kStop);
    assert(payload->target_device_id == "PI-INPUT-01");
    assert(payload->component_id == "input_conveyor");
    assert(payload->params.at("reason") == "BOX_DETECTED");
    assert(orchestrator.StateMachine().ActiveWorks().empty());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceCommandTopic("PI-INPUT-01"), stop).IsSuccess());
}

void TestInvalidOrderAndDispatchFailureStaysProcessReady() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-BOX-2", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());
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
    assert(product_result.commands.size() == 1);
    assert(orchestrator.ConfirmDispatch(product_result.commands.front()).Applied());
    const auto pickup_ready = orchestrator.Handle(Status("MSG-LT-PICKUP-2", "PI-LT-01", "PICKUP_READY_A"));
    assert(pickup_ready.commands.size() == 1);
    assert(orchestrator.FailDispatch(pickup_ready.commands.front(), "gripper is offline").Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
    assert(orchestrator.AcceptsNewWork());
}

void TestInputFailureWithoutWorkIdPreservesTheProcess() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-BOX-3", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());
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
    assert(!result.handled);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
}

void TestOfflineStatusPreservesTheProcess() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-BOX-4", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());
    const auto offline = Message("MSG-INPUT-OFFLINE", mqtt::MessageType::kDeviceStatus, "PI-INPUT-01",
                                 mqtt::DeviceStatusPayload{
                                     .status = mqtt::ConnectionState::kOffline,
                                     .current_state = "DISCONNECTED",
                                     .job_id = std::nullopt,
                                     .error_code = std::string("ERR-MQTT-DISCONNECTED"),
                                 });
    const auto result = orchestrator.Handle(offline);
    assert(!result.handled);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
}

void TestEveryConfiguredNodeFailurePreservesTheProcess() {
    struct FailureCase {
        std::string_view device_id;
        mqtt::ConnectionState connection;
    };
    constexpr std::array failures{
        FailureCase{ "PI-INPUT-01", mqtt::ConnectionState::kUartError },
        FailureCase{ "PI-VISION-01", mqtt::ConnectionState::kRtspError },
        FailureCase{ "PI-GRIPPER-01", mqtt::ConnectionState::kMqttError },
        FailureCase{ "PI-SORTING-01", mqtt::ConnectionState::kUartError },
        FailureCase{ "PI-LT-01", mqtt::ConnectionState::kUartError },
    };

    for (std::size_t index = 0; index < failures.size(); ++index) {
        central_server::ProcessOrchestrator orchestrator({ .enabled = true });
        assert(orchestrator.BeginWork("MSG-NODE-WORK-" + std::to_string(index), kWorkId, "PI-INPUT-01", kTimestamp)
                   .transition.Applied());
        const auto result = orchestrator.Handle(Status("MSG-NODE-FAILURE-" + std::to_string(index),
                                                       std::string(failures[index].device_id), "STOPPED",
                                                       failures[index].connection, std::nullopt));
        assert(!result.handled);
        assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
        assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
    }
}

void TestHealthyStoppedNodesDoNotFailTheProcess() {
    constexpr std::array device_ids{
        "PI-INPUT-01", "PI-VISION-01", "PI-GRIPPER-01", "PI-SORTING-01", "PI-LT-01",
    };
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-HEALTHY-IDLE-WORK", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());

    for (std::size_t index = 0; index < device_ids.size(); ++index) {
        const auto result = orchestrator.Handle(Status("MSG-HEALTHY-IDLE-" + std::to_string(index), device_ids[index],
                                                       "STOPPED", mqtt::ConnectionState::kOnline, std::nullopt));
        assert(!result.handled);
        assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
        assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
    }
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

void TestDeviceStatusDoesNotMutateCentralProcessState() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-ESTOP-WORK", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());

    const auto emergency = orchestrator.Handle(
        Status("MSG-ESTOP-INPUT", "PI-INPUT-01", "EMERGENCY_STOP", mqtt::ConnectionState::kOnline, std::nullopt));
    assert(!emergency.handled);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);

    const auto unavailable = orchestrator.Handle(
        Status("MSG-UART-SORTING", "PI-SORTING-01", "FAULT", mqtt::ConnectionState::kUartError, std::nullopt));
    assert(!unavailable.handled);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
}

void TestFailedSystemCommandsPreserveRequestedProcessStates() {
    central_server::ProcessOrchestrator start_failure({ .enabled = true });
    assert(start_failure.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    assert(start_failure
               .FailSystemCommand(mqtt::ControlCommand::kStart, mqtt::CommandResult::kRejected,
                                  "input node rejected START")
               .disposition == central_server::TransitionDisposition::kDuplicate);
    assert(start_failure.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);

    central_server::ProcessOrchestrator stop_failure({ .enabled = true });
    assert(stop_failure.BeginWork("MSG-STOP-WORK", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());
    assert(stop_failure.ApplySystemCommand(mqtt::ControlCommand::kStop).Applied());
    assert(stop_failure
               .FailSystemCommand(mqtt::ControlCommand::kStop, mqtt::CommandResult::kFailed, "input node did not stop")
               .disposition == central_server::TransitionDisposition::kDuplicate);
    assert(stop_failure.StateMachine().SystemState() == central_server::ProcessSystemState::kStopped);
    assert(stop_failure.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kStopped);

    central_server::ProcessOrchestrator recovery_failure({ .enabled = true });
    assert(recovery_failure.BeginWork("MSG-RECOVERY-WORK", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());
    assert(recovery_failure.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop).Applied());
    assert(recovery_failure.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(
        recovery_failure
            .FailSystemCommand(mqtt::ControlCommand::kRecovery, mqtt::CommandResult::kTimeout, "safety reset timed out")
            .disposition == central_server::TransitionDisposition::kDuplicate);
    assert(recovery_failure.StateMachine().SystemState() == central_server::ProcessSystemState::kRecovery);
    assert(recovery_failure.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kRecovering);

    central_server::ProcessOrchestrator emergency_failure({ .enabled = true });
    assert(emergency_failure.BeginWork("MSG-ESTOP-FAIL-WORK", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());
    assert(emergency_failure.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop).Applied());
    assert(emergency_failure
               .FailSystemCommand(mqtt::ControlCommand::kEmergencyStop, mqtt::CommandResult::kFailed,
                                  "one device did not confirm ESTOP")
               .disposition == central_server::TransitionDisposition::kDuplicate);
    assert(emergency_failure.StateMachine().SystemState() == central_server::ProcessSystemState::kEmergencyStop);
    assert(emergency_failure.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kEmergencyStopped);
}

void TestCommandTimeoutOnlyFailsIdentifiableWork() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    const auto begin = orchestrator.BeginWork("MSG-TIMEOUT-WORK", kWorkId, "PI-INPUT-01", kTimestamp);
    assert(begin.transition.Applied());
    assert(begin.commands.size() == 1);

    assert(orchestrator.FailDispatch(begin.commands.front(), "STOP command timed out").Applied());
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kFailed);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
}

void TestSystemCommandTimeoutDoesNotMutateActiveWorks() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-SYSTEM-TIMEOUT-WORK", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());

    const auto timed_out = orchestrator.FailSystemCommand(mqtt::ControlCommand::kStop, mqtt::CommandResult::kTimeout,
                                                          "STOP command timed out waiting for PI-INPUT-01");
    assert(timed_out.disposition == central_server::TransitionDisposition::kDuplicate);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kInputDetected);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->failure_reason.empty());
}

void TestFailedDeviceDoesNotBlockNewWork() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    const auto begin = orchestrator.BeginWork("MSG-INPUT-FAULT", kWorkId, "PI-INPUT-01", kTimestamp);
    assert(begin.transition.Applied() && begin.commands.size() == 1);
    assert(orchestrator.FailDispatch(begin.commands.front(), "input conveyor stop timed out").Applied());
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);
    assert(orchestrator.AcceptsNewWork());
    assert(orchestrator.AcceptsInputWorkCreation());

    assert(!orchestrator
                .Handle(
                    Status("MSG-INPUT-HEALTHY", "PI-INPUT-01", "STOPPED", mqtt::ConnectionState::kOnline, std::nullopt))
                .handled);
    assert(orchestrator.AcceptsNewWork());
}

void TestRecoveryPersistenceFailureKeepsMemoryStateAndPendingCommands() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    const auto begin = orchestrator.BeginWork("MSG-RECOVERY-ATOMIC", kWorkId, "PI-INPUT-01", kTimestamp);
    assert(begin.transition.Applied() && begin.commands.size() == 1);
    central_server::ProcessCommandTracker tracker;
    assert(tracker.Track(begin.commands.front()));
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kEmergencyStop).Applied());
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());

    std::vector<central_server::WorkProcessSnapshot> persisted_works;
    const auto transition = orchestrator.CommitSystemRecovery(
        [&persisted_works](const std::vector<central_server::WorkProcessSnapshot>& active_works) {
            persisted_works = active_works;
            return false;
        });

    assert(transition.disposition == central_server::TransitionDisposition::kRejected);
    assert(persisted_works.size() == 1);
    assert(persisted_works.front().work_id == kWorkId);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRecovery);
    assert(orchestrator.StateMachine().ActiveWorks().size() == 1);
    assert(tracker.PendingCount() == 1);
}

void TestRecoveryRestartStaysRecovering() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    const auto restore =
        orchestrator.RestoreAfterServerRestart(central_server::ProcessSystemState::kRecovery,
                                               { central_server::WorkProcessSnapshot{
                                                   .work_id = kWorkId,
                                                   .stage = central_server::WorkStage::kRecovering,
                                                   .suspended_stage = central_server::WorkStage::kVisionProcessing,
                                                   .destination = "1",
                                                   .last_source_id = "PI-VISION-01",
                                               } },
                                               {}, 12, { "MSG-BEFORE-RECOVERY" });

    assert(restore.restored);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRecovery);
    assert(orchestrator.StateMachine().ActiveWorks().size() == 1);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kRecovering);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->suspended_stage ==
           central_server::WorkStage::kVisionProcessing);
    assert(orchestrator.PreviewSystemCommand(mqtt::ControlCommand::kStart).disposition ==
           central_server::TransitionDisposition::kRejected);
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
    assert(result.commands.size() == 1);
    assert(orchestrator.ConfirmDispatch(result.commands.front()).Applied());
    const auto pickup_ready = orchestrator.Handle(Status("MSG-LT-PICKUP-RESTORED", "PI-LT-01", "PICKUP_READY_A"));
    assert(pickup_ready.commands.size() == 1);
    const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(pickup_ready.commands.front().message);
    assert(command != nullptr);
    assert(command->params.at("targetPose").at("x") == 150.0);
    assert(command->params.at("targetPose").at("yawDeg") == 10.0);

    assert(orchestrator.FailDispatch(pickup_ready.commands.front(), "gripper is offline").Applied());
    assert(orchestrator.GripperTargets().contains(kWorkId));
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRecovery).Applied());
    assert(orchestrator.CompleteSystemRecovery().Applied());
    assert(!orchestrator.StateMachine().FindWork(kWorkId).has_value());
    assert(!orchestrator.GripperTargets().contains(kWorkId));
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
    assert(result.commands.size() == 1);
    assert(orchestrator.ConfirmDispatch(result.commands.front()).Applied());
    const auto pickup_ready = orchestrator.Handle(Status("MSG-LT-PICKUP-NO-HOMOGRAPHY", "PI-LT-01", "PICKUP_READY_A"));
    assert(pickup_ready.commands.size() == 1);
    const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(pickup_ready.commands.front().message);
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

    assert(orchestrator.BeginWork("MSG-REPLACEMENT-BOX", kReplacementWorkId, "PI-INPUT-01", kTimestamp)
               .transition.Applied());
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

void TestOrchestratorRejectsASecondActiveWork() {
    central_server::ProcessOrchestrator orchestrator({ .enabled = true });
    assert(orchestrator.BeginWork("MSG-FIRST-WORK", kWorkId, "PI-INPUT-01", kTimestamp).transition.Applied());

    const auto second = orchestrator.BeginWork("MSG-SECOND-WORK", kQueuedWorkId, "PI-INPUT-01", kTimestamp);
    assert(second.transition.disposition == central_server::TransitionDisposition::kRejected);
    assert(second.commands.empty());
    assert(!orchestrator.StateMachine().FindWork(kQueuedWorkId).has_value());
}

void TestProcessCommandTrackerRestoresPendingCommand() {
    central_server::ProcessOrchestrator orchestrator;
    const auto begin = orchestrator.BeginWork("MSG-TRACKER-BOX", kWorkId, "PI-INPUT-01", kTimestamp);
    assert(begin.transition.Applied() && begin.commands.size() == 1);

    auto first = begin.commands.front();
    first.message.message_id = "PROCESS-9";
    mqtt::GetPayload<mqtt::ControlCommandPayload>(first.message)->request_id = first.message.message_id;
    auto second = first;
    second.message.message_id = "PROCESS-10";
    mqtt::GetPayload<mqtt::ControlCommandPayload>(second.message)->request_id = second.message.message_id;

    central_server::ProcessCommandTracker tracker;
    assert(tracker.Track(second));
    assert(tracker.Track(first));
    assert(tracker.MarkDispatched(first.message.message_id));
    assert(tracker.MarkDispatched(second.message.message_id));

    const auto saved = tracker.PendingCommands();
    assert(saved.size() == 2);
    assert(saved.front().message.message_id == "PROCESS-9");
    assert(saved.back().message.message_id == "PROCESS-10");
    assert(saved.front().dispatch_confirmed);

    central_server::ProcessCommandTracker restored;
    assert(restored.Restore(saved));
    assert(restored.PendingCount() == 2);
    const auto processing = Message("MSG-TRACKER-PROCESSING", mqtt::MessageType::kCommandResponse, "PI-INPUT-01",
                                    mqtt::CommandResponsePayload{
                                        .request_id = first.message.message_id,
                                        .command = mqtt::ControlCommand::kStop,
                                        .result = mqtt::CommandResult::kProcessing,
                                        .error_code = std::nullopt,
                                        .message = "input stop accepted",
                                    });
    assert(!restored.HandleResponse(processing).has_value());
    assert(restored.PendingCount() == 2);
    const auto response = Message("MSG-TRACKER-RESPONSE", mqtt::MessageType::kCommandResponse, "PI-INPUT-01",
                                  mqtt::CommandResponsePayload{
                                      .request_id = first.message.message_id,
                                      .command = mqtt::ControlCommand::kStop,
                                      .result = mqtt::CommandResult::kDuplicated,
                                      .error_code = std::nullopt,
                                      .message = "already stopped",
                                  });
    const auto completed = restored.HandleResponse(response);
    assert(completed.has_value());
    assert(completed->message.message_id == first.message.message_id);
    assert(restored.PendingCount() == 1);
}

void TestLineTracerLoadOnStartsTransportOnlyFromSorting() {
    for (const auto state : { "load_on_a", "LOAD_ON_B", "LOAD_ON_C" }) {
        central_server::ProcessOrchestrator orchestrator;
        const auto restored = orchestrator.RestoreAfterServerRestart(central_server::ProcessSystemState::kRunning,
                                                                     { central_server::WorkProcessSnapshot{
                                                                         .work_id = kWorkId,
                                                                         .stage = central_server::WorkStage::kSorting,
                                                                         .last_source_id = "PI-SORTING-01",
                                                                     } },
                                                                     {}, 0, {});
        assert(restored.restored);
        assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kRestart).Applied());

        const auto following_line = orchestrator.Handle(Status("MSG-LT-FOLLOWING-LINE", "PI-LT-01", "FOLLOWING_LINE"));
        assert(!following_line.handled);
        assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kSorting);

        const auto load_on =
            orchestrator.Handle(Status("MSG-LT-LOAD-ON-" + std::string(state), "PI-LT-01", std::string(state)));
        assert(load_on.transition.Applied());
        assert(load_on.transition.previous_stage == central_server::WorkStage::kSorting);
        assert(load_on.transition.current_stage == central_server::WorkStage::kTransporting);

        const auto delayed_sorting =
            orchestrator.Handle(Status("MSG-SORTING-DELAYED-" + std::string(state), "PI-SORTING-01", "CYCLE_COMPLETE"));
        assert(delayed_sorting.transition.disposition == central_server::TransitionDisposition::kDuplicate);
        assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kTransporting);
    }
}

void TestLineTracerBypassRunsGripperAndSortingToCompletion() {
    central_server::ProcessOrchestrator orchestrator({
        .line_tracer_enabled = false,
    });
    const auto restored =
        orchestrator.RestoreAfterServerRestart(central_server::ProcessSystemState::kRunning,
                                               { central_server::WorkProcessSnapshot{
                                                   .work_id = kWorkId,
                                                   .stage = central_server::WorkStage::kBarcodeRecognized,
                                                   .last_source_id = "PI-VISION-01",
                                               } },
                                               {}, 0, {});
    assert(restored.restored);
    assert(orchestrator.ApplySystemCommand(mqtt::ControlCommand::kStart).Applied());
    const auto stale_line_tracer = orchestrator.Handle(
        Status("MSG-NO-LT-STALE", "PI-LT-01", "OFFLINE", mqtt::ConnectionState::kOffline, std::nullopt));
    assert(!stale_line_tracer.handled);
    assert(orchestrator.StateMachine().SystemState() == central_server::ProcessSystemState::kRunning);

    const auto product =
        orchestrator.Handle(Message("MSG-NO-LT-PRODUCT", mqtt::MessageType::kProductInfo, "central-server",
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
                                    }));
    assert(product.transition.Applied() && product.commands.size() == 1);
    const auto* gripper = mqtt::GetPayload<mqtt::ControlCommandPayload>(product.commands.front().message);
    assert(gripper != nullptr && gripper->target_device_id == "PI-GRIPPER-01");
    assert(orchestrator.ConfirmDispatch(product.commands.front()).Applied());

    assert(
        orchestrator.Handle(Status("MSG-NO-LT-GRIPPER-START", "PI-GRIPPER-01", "TRANSFERRING")).transition.Applied());
    const auto gripper_done = orchestrator.HandleCommandCompletion(
        product.commands.front(), SuccessResponse("MSG-NO-LT-GRIPPER-DONE", "PI-GRIPPER-01", product.commands.front()));
    assert(gripper_done.transition.Applied() && gripper_done.commands.size() == 1);
    assert(orchestrator.ConfirmDispatch(gripper_done.commands.front()).Applied());
    const auto destination_done = orchestrator.HandleCommandCompletion(
        gripper_done.commands.front(),
        SuccessResponse("MSG-NO-LT-SORTING-DESTINATION-DONE", "PI-SORTING-01", gripper_done.commands.front()));
    assert(destination_done.commands.size() == 1);
    assert(orchestrator.ConfirmDispatch(destination_done.commands.front()).Applied());
    const auto sorting_start_done = orchestrator.HandleCommandCompletion(
        destination_done.commands.front(),
        SuccessResponse("MSG-NO-LT-SORTING-START-DONE", "PI-SORTING-01", destination_done.commands.front()));
    assert(sorting_start_done.commands.size() == 1);
    assert(orchestrator.ConfirmDispatch(sorting_start_done.commands.front()).Applied());
    assert(orchestrator.Handle(Status("MSG-NO-LT-SORTING-START", "PI-SORTING-01", "ROUTING")).transition.Applied());

    const auto disabled_load_on = orchestrator.Handle(Status("MSG-NO-LT-LOAD-ON", "PI-LT-01", "LOAD_ON_C"));
    assert(!disabled_load_on.handled);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kSorting);

    const auto sorting_done = orchestrator.Handle(Status("MSG-NO-LT-SORTING-DONE", "PI-SORTING-01", "CYCLE_COMPLETE"));
    assert(sorting_done.transition.Applied());
    assert(sorting_done.transition.current_stage == central_server::WorkStage::kCompleted);
    assert(orchestrator.StateMachine().FindWork(kWorkId)->stage == central_server::WorkStage::kCompleted);
}

}  // namespace

int main() {
    TestOnlyInputNodeCanCreateWork();
    TestProcessCommandIdsAreScopedToProcessEpoch();
    TestEventFlowCreatesCommandsForEachNode();
    TestInputDetectionSafetyStopDoesNotCreateWork();
    TestInvalidOrderAndDispatchFailureStaysProcessReady();
    TestInputFailureWithoutWorkIdPreservesTheProcess();
    TestOfflineStatusPreservesTheProcess();
    TestEveryConfiguredNodeFailurePreservesTheProcess();
    TestHealthyStoppedNodesDoNotFailTheProcess();
    TestFailedWorkIsDiscardedAfterServerRestart();
    TestDeviceStatusDoesNotMutateCentralProcessState();
    TestFailedSystemCommandsPreserveRequestedProcessStates();
    TestCommandTimeoutOnlyFailsIdentifiableWork();
    TestSystemCommandTimeoutDoesNotMutateActiveWorks();
    TestFailedDeviceDoesNotBlockNewWork();
    TestRecoveryPersistenceFailureKeepsMemoryStateAndPendingCommands();
    TestRecoveryRestartStaysRecovering();
    TestRestoredHomographyTargetCreatesGripperCommand();
    TestDisabledHomographyDiscardsRestoredTarget();
    TestChangedCalibrationDiscardsRestoredTarget();
    TestOrchestratorRejectsASecondActiveWork();
    TestProcessCommandTrackerRestoresPendingCommand();
    TestLineTracerLoadOnStartsTransportOnlyFromSorting();
    TestLineTracerBypassRunsGripperAndSortingToCompletion();
    return 0;
}
