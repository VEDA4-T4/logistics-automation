#include "logistics/central_server/process_state_store.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#ifndef LOGISTICS_TEST_MIGRATION_DIR
#define LOGISTICS_TEST_MIGRATION_DIR "central-server-rpi/db/migrations"
#endif

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

constexpr auto kWorkId = "d8e9b2be-bfc0-471c-9000-590123412345";
constexpr auto kSecondWorkId = "a8e9b2be-bfc0-471c-9000-590123412346";

[[nodiscard]] mqtt::MqttMessage FailedCompletion(std::string_view work_id = kWorkId) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "RECOVERY-FAILED-" + std::string(work_id),
        .message_type = mqtt::MessageType::kWorkCompleted,
        .source_id = "central-server",
        .timestamp = "2026-08-13T01:00:00Z",
        .data =
            mqtt::WorkCompletedPayload{
                .work_id = std::string(work_id),
                .result = "FAILED",
                .message = "CANCELLED_BY_RECOVERY",
            },
    };
}

[[nodiscard]] mqtt::MqttMessage WorkCreated() {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "WORK-" + std::string(kWorkId),
        .message_type = mqtt::MessageType::kWorkCreated,
        .source_id = "central-server",
        .timestamp = "2026-08-13T01:00:00Z",
        .data = mqtt::WorkCreatedPayload{ .work_id = kWorkId },
    };
}

[[nodiscard]] mqtt::MqttMessage ControlCommand(std::string request_id, mqtt::ControlCommand command) {
    return { .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
             .message_id = request_id,
             .message_type = mqtt::MessageType::kControlCommand,
             .source_id = "control-center",
             .timestamp = "2026-08-13T01:00:00Z",
             .data = mqtt::ControlCommandPayload{ .request_id = std::move(request_id),
                                                  .command = command,
                                                  .target_device_id = "SYSTEM",
                                                  .component_id = {},
                                                  .params = mqtt::Json::object() } };
}

[[nodiscard]] mqtt::MqttMessage CommandResponse(std::string source, std::string message_id, std::string request_id,
                                                mqtt::ControlCommand command, mqtt::CommandResult result) {
    return { .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
             .message_id = std::move(message_id),
             .message_type = mqtt::MessageType::kCommandResponse,
             .source_id = std::move(source),
             .timestamp = "2026-08-13T01:00:01Z",
             .data = mqtt::CommandResponsePayload{ .request_id = std::move(request_id),
                                                   .command = command,
                                                   .result = result,
                                                   .error_code = std::string("ERR-DEVICE"),
                                                   .message = "device failed" } };
}

[[nodiscard]] std::filesystem::path TemporaryDatabasePath() {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("logistics-process-state-" + std::to_string(value) + ".db");
}

void TestMqttDeliveryOutboxSurvivesRestartAndDeduplicates() {
    const auto path = TemporaryDatabasePath();
    const auto topic = mqtt::QtEventTopic("control-center");
    const auto message = FailedCompletion();
    {
        central_server::Database database;
        assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
        assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
        central_server::ProcessStateStore store(database);
        assert(store.EnqueueMqttDelivery(topic, message, 1000).ok());
        assert(store.EnqueueMqttDelivery(topic, message, 1001).ok());
    }
    {
        central_server::Database database;
        assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
        assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
        central_server::ProcessStateStore store(database);
        std::vector<central_server::PendingMqttDelivery> pending;
        assert(store.LoadPendingMqttDeliveries(pending).ok());
        assert(pending.size() == 1);
        assert(pending.front().topic == topic);
        assert(pending.front().message.message_id == message.message_id);
        assert(store.RemoveMqttDelivery(topic, message.message_id).ok());
        assert(store.LoadPendingMqttDeliveries(pending).ok());
        assert(pending.empty());
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void TestVisionWorkCreatedDeliveryClassification() {
    const auto message = WorkCreated();
    assert(central_server::IsVisionWorkCreatedDelivery(
        { .topic = mqtt::DeviceCommandTopic("PI-VISION-01"), .message = message }, "PI-VISION-01"));
    assert(!central_server::IsVisionWorkCreatedDelivery(
        { .topic = mqtt::QtEventTopic("control-center"), .message = message }, "PI-VISION-01"));
    assert(!central_server::IsVisionWorkCreatedDelivery(
        { .topic = mqtt::DeviceCommandTopic("PI-INPUT-01"), .message = message }, "PI-VISION-01"));
}

std::int64_t Scalar(central_server::Database& database, std::string_view sql) {
    central_server::Statement statement;
    assert(database.Prepare(sql, statement).ok());
    bool row = false;
    assert(statement.Step(row).ok() && row);
    return statement.ColumnInt64(0);
}

void TestCommandManagerAndSystemCommandSnapshotRoundTrip() {
    const auto path = TemporaryDatabasePath();
    central_server::CommandManager manager;
    constexpr auto request_id = "c8e9b2be-bfc0-471c-9000-590123412345";
    const auto command = ControlCommand(request_id, mqtt::ControlCommand::kRecovery);
    assert(manager.TrackCommand(command, { "PI-01", "PI-02" }));
    assert(manager
               .HandleResponse(CommandResponse("PI-01", "RESP-1", request_id, mqtt::ControlCommand::kRecovery,
                                               mqtt::CommandResult::kFailed))
               .message.has_value());
    const auto snapshot = manager.Snapshot();
    {
        central_server::Database database;
        assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
        assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
        central_server::ProcessStateStore store(database);
        const std::vector works{ central_server::WorkProcessSnapshot{ .work_id = kWorkId,
                                                                      .stage =
                                                                          central_server::WorkStage::kInputDetected,
                                                                      .suspended_stage = std::nullopt,
                                                                      .destination = {},
                                                                      .last_source_id = "PI-INPUT-01",
                                                                      .failure_reason = {} } };
        const auto save_status = store.Save(central_server::ProcessSystemState::kRecovery, 0, works, {}, {}, 1000, {},
                                            {}, snapshot, { { request_id, mqtt::ControlCommand::kRecovery } });
        assert(save_status.ok());
        assert(Scalar(database, "SELECT count(*) FROM process_runtime_state") == 1);
        assert(database.Checkpoint().ok());
        assert(database.Close().ok());
    }
    {
        central_server::Database database;
        assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
        assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
        central_server::ProcessStateStore store(database);
        assert(Scalar(database, "SELECT count(*) FROM process_runtime_state") == 1);
        std::optional<central_server::StoredProcessState> stored;
        const auto load_status = store.Load(stored);
        assert(load_status.ok() && stored.has_value());
        assert(stored->command_manager.pending.size() == 1);
        assert(stored->command_manager.pending.front().failure.has_value());
        assert(stored->pending_system_commands.at(request_id) == mqtt::ControlCommand::kRecovery);
        central_server::CommandManager restored;
        assert(restored.Restore(stored->command_manager));
        const auto final = restored.HandleResponse(CommandResponse(
            "PI-02", "RESP-2", request_id, mqtt::ControlCommand::kRecovery, mqtt::CommandResult::kSuccess));
        assert(final.message.has_value());
        assert(mqtt::GetPayload<mqtt::CommandResponsePayload>(*final.message)->result == mqtt::CommandResult::kFailed);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void TestMqttDeliveryOutboxPreservesInsertionOrderForEqualTimestamps() {
    const auto path = TemporaryDatabasePath();
    central_server::Database database;
    assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
    assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
    central_server::ProcessStateStore store(database);
    std::vector<central_server::PendingMqttDelivery> deliveries;
    for (int sequence = 0; sequence < 12; ++sequence) {
        auto message = FailedCompletion();
        message.message_id = "FIFO-" + std::to_string(sequence);
        deliveries.push_back({ .topic = mqtt::QtEventTopic("control-center"), .message = std::move(message) });
    }
    assert(store.EnqueueMqttDeliveries(deliveries, 1000).ok());

    std::vector<central_server::PendingMqttDelivery> pending;
    assert(store.LoadPendingMqttDeliveries(pending).ok());
    assert(pending.size() == deliveries.size());
    for (std::size_t index = 0; index < pending.size(); ++index) {
        assert(pending[index].message.message_id == "FIFO-" + std::to_string(index));
    }

    assert(database.Close().ok());
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void TestVisionAssignmentAcknowledgementClassification() {
    const mqtt::MqttMessage acknowledged{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "STATUS-WORK-ASSIGNED-01",
        .message_type = mqtt::MessageType::kDeviceStatus,
        .source_id = "PI-VISION-01",
        .timestamp = "2026-08-13T01:00:00Z",
        .data =
            mqtt::DeviceStatusPayload{
                .status = mqtt::ConnectionState::kOnline,
                .current_state = "WORK_ASSIGNED",
                .job_id = std::string(kWorkId),
            },
    };
    assert(central_server::AcknowledgedVisionWorkId(acknowledged, "PI-VISION-01") == kWorkId);
    assert(!central_server::AcknowledgedVisionWorkId(acknowledged, "PI-VISION-02").has_value());
    auto not_assigned = acknowledged;
    mqtt::GetPayload<mqtt::DeviceStatusPayload>(not_assigned)->current_state = "ONLINE";
    assert(!central_server::AcknowledgedVisionWorkId(not_assigned, "PI-VISION-01").has_value());
}

void TestSnapshotAndDeliveryBatchRollbackTogether() {
    const auto path = TemporaryDatabasePath();
    central_server::Database database;
    assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
    assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
    central_server::ProcessStateStore store(database);
    const auto input_detected = central_server::WorkProcessSnapshot{
        .work_id = kWorkId,
        .stage = central_server::WorkStage::kInputDetected,
        .last_source_id = "PI-INPUT-01",
    };
    assert(store.Save(central_server::ProcessSystemState::kRunning, 1, { input_detected }, {}, {}, 1000).ok());

    auto vision_assigned = input_detected;
    vision_assigned.stage = central_server::WorkStage::kVisionAssigned;
    const auto created = WorkCreated();
    const std::vector invalid_batch{
        central_server::PendingMqttDelivery{
            .topic = mqtt::DeviceCommandTopic("PI-VISION-01"),
            .message = created,
        },
        central_server::PendingMqttDelivery{ .topic = {}, .message = created },
    };
    assert(
        !store.Save(central_server::ProcessSystemState::kRunning, 2, { vision_assigned }, {}, {}, 1001, invalid_batch)
             .ok());

    std::optional<central_server::StoredProcessState> restored;
    assert(store.Load(restored).ok());
    assert(restored.has_value());
    assert(restored->message_sequence == 1);
    assert(restored->works.size() == 1);
    assert(restored->works.front().stage == central_server::WorkStage::kInputDetected);
    std::vector<central_server::PendingMqttDelivery> pending;
    assert(store.LoadPendingMqttDeliveries(pending).ok());
    assert(pending.empty());

    const std::vector valid_batch{ invalid_batch.front() };
    assert(store.Save(central_server::ProcessSystemState::kRunning, 2, { vision_assigned }, {}, {}, 1002, valid_batch)
               .ok());
    assert(store.Load(restored).ok());
    assert(restored->message_sequence == 2);
    assert(restored->works.front().stage == central_server::WorkStage::kVisionAssigned);
    assert(store.LoadPendingMqttDeliveries(pending).ok());
    assert(pending.size() == 1);

    assert(database.Close().ok());
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void TestRecoveryCommitReplacesRuntimeAndOutboxAtomically() {
    const auto path = TemporaryDatabasePath();
    central_server::Database database;
    assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
    assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
    central_server::ProcessStateStore store(database);

    const central_server::WorkProcessSnapshot work{
        .work_id = kWorkId,
        .stage = central_server::WorkStage::kRecovering,
        .suspended_stage = central_server::WorkStage::kVisionAssigned,
        .last_source_id = "PI-VISION-01",
    };
    auto second_work = work;
    second_work.work_id = kSecondWorkId;
    central_server::ProcessCommandIntent process_command{
        .message = ControlCommand("PROCESS-RECOVERY-PENDING", mqtt::ControlCommand::kStop),
        .work_id = kWorkId,
        .dispatch_confirmed = true,
    };
    auto* process_payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(process_command.message);
    assert(process_payload != nullptr);
    process_payload->target_device_id = "PI-INPUT-01";

    constexpr auto recovery_request_id = "RECOVERY-SYSTEM-PENDING";
    central_server::CommandManager manager;
    assert(manager.TrackCommand(ControlCommand(recovery_request_id, mqtt::ControlCommand::kRecovery),
                                { "PI-INPUT-01", "PI-VISION-01" }));
    const auto created = WorkCreated();
    assert(store
               .Save(central_server::ProcessSystemState::kRecovery, 41, { work, second_work }, {}, { process_command },
                     1000, { { .topic = mqtt::DeviceCommandTopic("PI-VISION-01"), .message = created } },
                     { "PRE-RECOVERY-EVENT" }, manager.Snapshot(),
                     { { recovery_request_id, mqtt::ControlCommand::kRecovery } })
               .ok());

    const central_server::PendingMqttDelivery completion{
        .topic = mqtt::QtEventTopic("control-center"),
        .message = FailedCompletion(),
    };
    const central_server::PendingMqttDelivery second_completion{
        .topic = mqtt::QtEventTopic("control-center"),
        .message = FailedCompletion(kSecondWorkId),
    };
    assert(store.CommitRecovery(42, 7, 1001, { completion, second_completion }).ok());

    std::optional<central_server::StoredProcessState> restored;
    assert(store.Load(restored).ok() && restored.has_value());
    assert(restored->system_state == central_server::ProcessSystemState::kStopped);
    assert(restored->message_sequence == 42);
    assert(restored->works.empty());
    assert(restored->gripper_targets.empty());
    assert(restored->pending_commands.empty());
    assert(restored->processed_message_ids.empty());
    assert(restored->command_manager.pending.empty());
    assert(restored->command_manager.completed_requests.empty());
    assert(restored->command_manager.message_sequence == 7);
    assert(restored->pending_system_commands.empty());

    std::vector<central_server::PendingMqttDelivery> pending;
    assert(store.LoadPendingMqttDeliveries(pending).ok());
    assert(pending.size() == 2);
    assert(pending.front().message.message_id == completion.message.message_id);
    const auto* completed = mqtt::GetPayload<mqtt::WorkCompletedPayload>(pending.front().message);
    assert(completed != nullptr);
    assert(completed->work_id == kWorkId);
    assert(completed->result == "FAILED");
    assert(completed->message == std::optional<std::string>{ "CANCELLED_BY_RECOVERY" });
    const auto* second_completed = mqtt::GetPayload<mqtt::WorkCompletedPayload>(pending.back().message);
    assert(second_completed != nullptr);
    assert(second_completed->work_id == kSecondWorkId);
    assert(second_completed->result == "FAILED");
    assert(second_completed->message == std::optional<std::string>{ "CANCELLED_BY_RECOVERY" });

    assert(database.Close().ok());
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void TestRecoveryCommitFailureRollsBackRuntimeAndOutbox() {
    const auto path = TemporaryDatabasePath();
    central_server::Database database;
    assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
    assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
    central_server::ProcessStateStore store(database);
    const central_server::WorkProcessSnapshot work{
        .work_id = kWorkId,
        .stage = central_server::WorkStage::kRecovering,
        .suspended_stage = central_server::WorkStage::kInputDetected,
        .last_source_id = "PI-INPUT-01",
    };
    const auto created = WorkCreated();
    assert(store
               .Save(central_server::ProcessSystemState::kRecovery, 7, { work }, {}, {}, 1000,
                     { { .topic = mqtt::DeviceCommandTopic("PI-VISION-01"), .message = created } })
               .ok());

    const std::vector invalid_terminal_batch{
        central_server::PendingMqttDelivery{
            .topic = mqtt::QtEventTopic("control-center"),
            .message = FailedCompletion(),
        },
        central_server::PendingMqttDelivery{
            .topic = {},
            .message = FailedCompletion(),
        },
    };
    assert(!store.CommitRecovery(8, 3, 1001, invalid_terminal_batch).ok());

    std::optional<central_server::StoredProcessState> restored;
    assert(store.Load(restored).ok() && restored.has_value());
    assert(restored->system_state == central_server::ProcessSystemState::kRecovery);
    assert(restored->message_sequence == 7);
    assert(restored->works.size() == 1);
    assert(restored->works.front().work_id == kWorkId);
    std::vector<central_server::PendingMqttDelivery> pending;
    assert(store.LoadPendingMqttDeliveries(pending).ok());
    assert(pending.size() == 1);
    assert(pending.front().message.message_id == created.message_id);

    assert(database.Close().ok());
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void TestSnapshotSurvivesRestartAndIsSafelySuspended() {
    const auto path = TemporaryDatabasePath();
    {
        central_server::Database database;
        assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
        assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
        central_server::ProcessStateStore store(database);
        const std::vector works{
            central_server::WorkProcessSnapshot{
                .work_id = kWorkId,
                .stage = central_server::WorkStage::kVisionProcessing,
                .suspended_stage = std::nullopt,
                .destination = "1",
                .last_source_id = "PI-VISION-01",
                .failure_reason = {},
            },
        };
        const std::unordered_map<std::string, central_server::GripperTarget> targets{
            { kWorkId,
              {
                  .x_mm = 125.5,
                  .y_mm = -42.25,
                  .z_mm = 950.0,
                  .yaw_deg = 17.5,
                  .box_length_mm = 400.0,
                  .box_width_mm = 200.0,
                  .box_height_mm = 150.0,
                  .coordinate_frame = "PI-GRIPPER-01_BASE",
                  .calibration_version = 3,
              } },
        };
        const std::vector pending_commands{
            central_server::ProcessCommandIntent{
                .message =
                    {
                        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                        .message_id = "PROCESS-central-server-43",
                        .message_type = mqtt::MessageType::kControlCommand,
                        .source_id = "central-server",
                        .timestamp = "2026-08-13T01:00:00Z",
                        .data =
                            mqtt::ControlCommandPayload{
                                .request_id = "PROCESS-central-server-43",
                                .command = mqtt::ControlCommand::kStop,
                                .target_device_id = "PI-INPUT-01",
                                .component_id = "input_conveyor",
                                .params = mqtt::Json{ { "workId", kWorkId } },
                            },
                    },
                .dispatched_event = std::nullopt,
                .work_id = kWorkId,
                .dispatch_confirmed = true,
            },
        };
        assert(store
                   .Save(central_server::ProcessSystemState::kRunning, 42, works, targets, pending_commands, 1000, {},
                         { "EVENT-1", "EVENT-2" })
                   .ok());
    }

    {
        central_server::Database database;
        assert(database.Open({ .path = path, .migration_dir = LOGISTICS_TEST_MIGRATION_DIR }).ok());
        assert(central_server::MigrationRunner::Apply(database, LOGISTICS_TEST_MIGRATION_DIR).ok());
        central_server::ProcessStateStore store(database);
        std::optional<central_server::StoredProcessState> stored;
        assert(store.Load(stored).ok());
        assert(stored.has_value());
        assert(stored->message_sequence == 42);
        assert(stored->works.size() == 1);
        assert(stored->gripper_targets.size() == 1);
        assert(stored->pending_commands.size() == 1);
        assert(stored->pending_commands.front().work_id == kWorkId);
        assert(stored->pending_commands.front().dispatch_confirmed);
        assert((stored->processed_message_ids == std::vector<std::string>{ "EVENT-1", "EVENT-2" }));
        const auto* pending = mqtt::GetPayload<mqtt::ControlCommandPayload>(stored->pending_commands.front().message);
        assert(pending != nullptr);
        assert(pending->request_id == "PROCESS-central-server-43");
        assert(pending->command == mqtt::ControlCommand::kStop);
        const auto& target = stored->gripper_targets.at(kWorkId);
        assert(target.x_mm == 125.5);
        assert(target.y_mm == -42.25);
        assert(target.z_mm == 950.0);
        assert(target.yaw_deg == 17.5);
        assert(target.coordinate_frame == "PI-GRIPPER-01_BASE");
        assert(target.calibration_version == 3);

        central_server::ProcessStateMachine machine;
        assert(machine.RestoreAfterServerRestart(stored->system_state, stored->works));
        assert(machine.SystemState() == central_server::ProcessSystemState::kStopped);
        const auto work = machine.FindWork(kWorkId);
        assert(work.has_value());
        assert(work->stage == central_server::WorkStage::kStopped);
        assert(work->suspended_stage == central_server::WorkStage::kVisionProcessing);
        assert(database.Checkpoint().ok());
        assert(database.Close().ok());
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

}  // namespace

int main() {
    TestSnapshotSurvivesRestartAndIsSafelySuspended();
    TestMqttDeliveryOutboxSurvivesRestartAndDeduplicates();
    TestMqttDeliveryOutboxPreservesInsertionOrderForEqualTimestamps();
    TestVisionWorkCreatedDeliveryClassification();
    TestVisionAssignmentAcknowledgementClassification();
    TestCommandManagerAndSystemCommandSnapshotRoundTrip();
    TestSnapshotAndDeliveryBatchRollbackTogether();
    TestRecoveryCommitReplacesRuntimeAndOutboxAtomically();
    TestRecoveryCommitFailureRollsBackRuntimeAndOutbox();
    return 0;
}
