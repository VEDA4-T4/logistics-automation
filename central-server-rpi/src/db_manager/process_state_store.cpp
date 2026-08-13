#include "logistics/central_server/process_state_store.hpp"

#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;

[[nodiscard]] std::optional<std::string> CommandRequestId(const mqtt::MqttMessage& message) {
    if (const auto* control = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return control->request_id;
    }
    if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        return destination->request_id;
    }
    return std::nullopt;
}

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

[[nodiscard]] DatabaseStatus BindGripperTarget(Statement& statement, std::string_view work_id,
                                               const GripperTarget& target, std::int64_t updated_at_ms) {
    auto status = statement.Bind(1, work_id);
    if (!status.ok() || !(status = statement.Bind(2, target.x_mm)).ok() ||
        !(status = statement.Bind(3, target.y_mm)).ok() || !(status = statement.Bind(4, target.z_mm)).ok() ||
        !(status = statement.Bind(5, target.yaw_deg)).ok() ||
        !(status = statement.Bind(6, target.box_length_mm)).ok() ||
        !(status = statement.Bind(7, target.box_width_mm)).ok() ||
        !(status = statement.Bind(8, target.box_height_mm)).ok() ||
        !(status = statement.Bind(9, target.coordinate_frame)).ok() ||
        !(status = statement.Bind(10, target.calibration_version)).ok()) {
        return status;
    }
    return statement.Bind(11, updated_at_ms);
}

[[nodiscard]] DatabaseStatus BindPendingCommand(Statement& statement, const ProcessCommandIntent& intent,
                                                std::int64_t updated_at_ms) {
    const auto request_id = CommandRequestId(intent.message);
    const auto encoded = mqtt::SerializeMessage(intent.message);
    if (!request_id.has_value() || request_id->empty() || !contracts::IsValidUuid(intent.work_id) ||
        !encoded.IsSuccess()) {
        return { DatabaseStatusCode::kInvalidArgument, "invalid pending process command" };
    }
    auto status = statement.Bind(1, *request_id);
    if (!status.ok() || !(status = statement.Bind(2, encoded.payload)).ok() ||
        !(status = statement.Bind(3, intent.work_id)).ok()) {
        return status;
    }
    status = intent.dispatched_event.has_value() ? statement.Bind(4, ToString(*intent.dispatched_event))
                                                 : statement.BindNull(4);
    if (!status.ok() || !(status = statement.Bind(5, intent.dispatch_confirmed ? 1 : 0)).ok()) {
        return status;
    }
    return statement.Bind(6, updated_at_ms);
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
        .gripper_targets = {},
        .pending_commands = {},
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

    Statement targets;
    status = database_.Prepare(
        "SELECT work_id,x_mm,y_mm,z_mm,yaw_deg,box_length_mm,box_width_mm,box_height_mm,"
        "coordinate_frame,calibration_version FROM process_gripper_target ORDER BY work_id",
        targets);
    if (!status.ok()) {
        return status;
    }
    while ((status = targets.Step(row)).ok() && row) {
        restored.gripper_targets.emplace(targets.ColumnText(0), GripperTarget{
                                                                    .x_mm = targets.ColumnDouble(1),
                                                                    .y_mm = targets.ColumnDouble(2),
                                                                    .z_mm = targets.ColumnDouble(3),
                                                                    .yaw_deg = targets.ColumnDouble(4),
                                                                    .box_length_mm = targets.ColumnDouble(5),
                                                                    .box_width_mm = targets.ColumnDouble(6),
                                                                    .box_height_mm = targets.ColumnDouble(7),
                                                                    .coordinate_frame = targets.ColumnText(8),
                                                                    .calibration_version = targets.ColumnInt(9),
                                                                });
    }
    if (!status.ok()) {
        return status;
    }

    Statement commands;
    status = database_.Prepare(
        "SELECT request_id,message_json,work_id,dispatched_event,dispatch_confirmed "
        "FROM process_command_outbox ORDER BY request_id",
        commands);
    if (!status.ok()) {
        return status;
    }
    while ((status = commands.Step(row)).ok() && row) {
        const auto decoded = mqtt::DeserializeMessage(commands.ColumnText(1));
        const std::string event_text = commands.ColumnText(3);
        const auto event = event_text.empty() ? std::optional<ProcessEventType>{} : ParseProcessEventType(event_text);
        const int dispatch_confirmed = commands.ColumnInt(4);
        if (!decoded.IsSuccess() || (!event_text.empty() && !event.has_value()) ||
            CommandRequestId(decoded.value) != std::optional<std::string>{ commands.ColumnText(0) } ||
            !contracts::IsValidUuid(commands.ColumnText(2)) || (dispatch_confirmed != 0 && dispatch_confirmed != 1)) {
            return { DatabaseStatusCode::kSqlError, "stored pending process command is invalid" };
        }
        restored.pending_commands.push_back({
            .message = decoded.value,
            .dispatched_event = event,
            .work_id = commands.ColumnText(2),
            .dispatch_confirmed = dispatch_confirmed != 0,
        });
    }
    if (!status.ok()) {
        return status;
    }
    output = std::move(restored);
    return DatabaseStatus::Ok();
}

DatabaseStatus ProcessStateStore::Save(ProcessSystemState system_state, std::uint64_t message_sequence,
                                       const std::vector<WorkProcessSnapshot>& works,
                                       const std::unordered_map<std::string, GripperTarget>& gripper_targets,
                                       const std::vector<ProcessCommandIntent>& pending_commands,
                                       std::int64_t updated_at_ms) {
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

    std::unordered_set<std::string_view> active_work_ids;
    active_work_ids.reserve(works.size());
    for (const auto& work : works) {
        active_work_ids.emplace(work.work_id);
    }
    Statement insert_target;
    status = database_.Prepare(
        "INSERT INTO process_gripper_target(work_id,x_mm,y_mm,z_mm,yaw_deg,box_length_mm,box_width_mm,"
        "box_height_mm,coordinate_frame,calibration_version,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        insert_target);
    if (!status.ok()) {
        return status;
    }
    for (const auto& [work_id, target] : gripper_targets) {
        if (!active_work_ids.contains(work_id)) {
            continue;
        }
        if (!(status = BindGripperTarget(insert_target, work_id, target, updated_at_ms)).ok() ||
            !(status = insert_target.Step(row)).ok() || !(status = insert_target.Reset()).ok()) {
            return status;
        }
    }

    if (!(status = database_.Execute("DELETE FROM process_command_outbox")).ok()) {
        return status;
    }
    Statement insert_command;
    status = database_.Prepare(
        "INSERT INTO process_command_outbox(request_id,message_json,work_id,dispatched_event,dispatch_confirmed,"
        "created_at_ms) VALUES(?,?,?,?,?,?)",
        insert_command);
    if (!status.ok()) {
        return status;
    }
    for (const auto& command : pending_commands) {
        if (!active_work_ids.contains(command.work_id)) {
            return { DatabaseStatusCode::kInvalidArgument, "pending command work is not active" };
        }
        if (!(status = BindPendingCommand(insert_command, command, updated_at_ms)).ok() ||
            !(status = insert_command.Step(row)).ok() || !(status = insert_command.Reset()).ok()) {
            return status;
        }
    }
    return transaction.Commit();
}

}  // namespace logistics::central_server
