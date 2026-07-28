#include "logistics/central_server/process_state_store.hpp"

#include <limits>
#include <string>
#include <utility>

namespace logistics::central_server {
namespace {

[[nodiscard]] DatabaseStatus BindWork(Statement& statement, const WorkProcessSnapshot& work,
                                      std::int64_t updated_at_ms) {
    auto status = statement.Bind(1, work.work_id);
    if (!status.ok()) {
        return status;
    }
    status = statement.Bind(2, ToString(work.stage));
    if (!status.ok()) {
        return status;
    }
    status =
        work.suspended_stage.has_value() ? statement.Bind(3, ToString(*work.suspended_stage)) : statement.BindNull(3);
    if (!status.ok() || !(status = statement.Bind(4, work.destination)).ok() ||
        !(status = statement.Bind(5, work.last_source_id)).ok() ||
        !(status = statement.Bind(6, work.failure_reason)).ok()) {
        return status;
    }
    return statement.Bind(7, updated_at_ms);
}

}  // namespace

DatabaseStatus ProcessStateStore::Load(std::optional<StoredProcessState>& output) {
    output.reset();
    Statement runtime;
    auto status =
        database_.Prepare("SELECT system_state,message_sequence FROM process_runtime_state WHERE id=1", runtime);
    if (!status.ok()) {
        return status;
    }
    bool row = false;
    if (!(status = runtime.Step(row)).ok() || !row) {
        return status;
    }
    const auto system_state = ParseProcessSystemState(runtime.ColumnText(0));
    const auto sequence = runtime.ColumnInt64(1);
    if (!system_state.has_value() || sequence < 0) {
        return { DatabaseStatusCode::kSqlError, "stored process runtime state is invalid" };
    }

    StoredProcessState restored{
        .system_state = *system_state,
        .message_sequence = static_cast<std::uint64_t>(sequence),
        .works = {},
    };
    Statement works;
    status = database_.Prepare(
        "SELECT work_id,stage,suspended_stage,destination,last_source_id,failure_reason "
        "FROM process_work_state ORDER BY work_id",
        works);
    if (!status.ok()) {
        return status;
    }
    while ((status = works.Step(row)).ok() && row) {
        const auto stage = ParseWorkStage(works.ColumnText(1));
        const std::string suspended_text = works.ColumnText(2);
        const auto suspended = suspended_text.empty() ? std::optional<WorkStage>{} : ParseWorkStage(suspended_text);
        if (!stage.has_value() || (!suspended_text.empty() && !suspended.has_value())) {
            return { DatabaseStatusCode::kSqlError, "stored work process stage is invalid" };
        }
        restored.works.push_back({
            .work_id = works.ColumnText(0),
            .stage = *stage,
            .suspended_stage = suspended,
            .destination = works.ColumnText(3),
            .last_source_id = works.ColumnText(4),
            .failure_reason = works.ColumnText(5),
        });
    }
    if (!status.ok()) {
        return status;
    }
    output = std::move(restored);
    return DatabaseStatus::Ok();
}

DatabaseStatus ProcessStateStore::Save(ProcessSystemState system_state, std::uint64_t message_sequence,
                                       const std::vector<WorkProcessSnapshot>& works, std::int64_t updated_at_ms) {
    if (updated_at_ms < 0 || message_sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return { DatabaseStatusCode::kInvalidArgument, "invalid process runtime snapshot" };
    }
    Transaction transaction(database_);
    if (!transaction.status().ok()) {
        return transaction.status();
    }

    Statement runtime;
    auto status = database_.Prepare(
        "INSERT INTO process_runtime_state(id,system_state,message_sequence,updated_at_ms) VALUES(1,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET system_state=excluded.system_state,"
        "message_sequence=excluded.message_sequence,updated_at_ms=excluded.updated_at_ms",
        runtime);
    if (!status.ok() || !(status = runtime.Bind(1, ToString(system_state))).ok() ||
        !(status = runtime.Bind(2, static_cast<std::int64_t>(message_sequence))).ok() ||
        !(status = runtime.Bind(3, updated_at_ms)).ok()) {
        return status;
    }
    bool row = false;
    if (!(status = runtime.Step(row)).ok() || !(status = database_.Execute("DELETE FROM process_work_state")).ok()) {
        return status;
    }

    Statement insert;
    status = database_.Prepare(
        "INSERT INTO process_work_state(work_id,stage,suspended_stage,destination,last_source_id,"
        "failure_reason,updated_at_ms) VALUES(?,?,?,?,?,?,?)",
        insert);
    if (!status.ok()) {
        return status;
    }
    for (const auto& work : works) {
        if (!(status = BindWork(insert, work, updated_at_ms)).ok() || !(status = insert.Step(row)).ok() ||
            !(status = insert.Reset()).ok()) {
            return status;
        }
    }
    return transaction.Commit();
}

}  // namespace logistics::central_server
