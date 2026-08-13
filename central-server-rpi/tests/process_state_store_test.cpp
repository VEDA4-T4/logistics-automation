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

[[nodiscard]] std::filesystem::path TemporaryDatabasePath() {
    const auto value = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("logistics-process-state-" + std::to_string(value) + ".db");
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
        assert(
            store.Save(central_server::ProcessSystemState::kRunning, 42, works, targets, pending_commands, 1000).ok());
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
    return 0;
}
