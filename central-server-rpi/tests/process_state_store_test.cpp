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
        assert(store.Save(central_server::ProcessSystemState::kRunning, 42, works, 1000).ok());
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
