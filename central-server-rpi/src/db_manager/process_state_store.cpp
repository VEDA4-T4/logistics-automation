#include "logistics/central_server/process_state_store.hpp"

#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

#include "logistics/contracts/mqtt_validation.hpp"

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;

[[nodiscard]] std::string EncodeStringList(const std::vector<std::string>& values) {
    return mqtt::Json(values).dump();
}

[[nodiscard]] std::optional<std::vector<std::string>> DecodeStringList(std::string_view encoded) {
    try {
        const auto json = mqtt::Json::parse(encoded);
        if (!json.is_array())
            return std::nullopt;
        return json.get<std::vector<std::string>>();
    } catch (const mqtt::Json::exception&) {
        return std::nullopt;
    }
}

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

bool IsVisionWorkCreatedDelivery(const PendingMqttDelivery& delivery, std::string_view vision_device_id) {
    return delivery.topic == mqtt::DeviceCommandTopic(vision_device_id) &&
           delivery.message.message_type == mqtt::MessageType::kWorkCreated &&
           mqtt::GetPayload<mqtt::WorkCreatedPayload>(delivery.message) != nullptr;
}

std::optional<std::string> AcknowledgedVisionWorkId(const mqtt::MqttMessage& message,
                                                    std::string_view vision_device_id) {
    if (message.source_id != vision_device_id || message.message_type != mqtt::MessageType::kDeviceStatus) {
        return std::nullopt;
    }
    const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message);
    if (status == nullptr || status->current_state != "WORK_ASSIGNED" || !status->job_id.has_value() ||
        status->job_id->empty()) {
        return std::nullopt;
    }
    return status->job_id;
}

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
        .processed_message_ids = {},
        .command_manager = {},
        .pending_system_commands = {},
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
    Statement command_runtime;
    status = database_.Prepare("SELECT message_sequence FROM command_manager_runtime WHERE id=1", command_runtime);
    if (!status.ok())
        return status;
    if ((status = command_runtime.Step(row)).ok() && row) {
        const auto sequence_value = command_runtime.ColumnInt64(0);
        if (sequence_value < 0)
            return { DatabaseStatusCode::kSqlError, "stored command manager sequence is invalid" };
        restored.command_manager.message_sequence = static_cast<std::uint64_t>(sequence_value);
    } else if (!status.ok())
        return status;
    Statement pending_command_manager;
    status = database_.Prepare(
        "SELECT request_id,message_json,expected_devices_json,completed_devices_json,"
        "response_message_ids_json,failure_json,deadline_at_ms FROM command_manager_pending",
        pending_command_manager);
    if (!status.ok())
        return status;
    while ((status = pending_command_manager.Step(row)).ok() && row) {
        const auto message = mqtt::DeserializeMessage(pending_command_manager.ColumnText(1));
        const auto expected = DecodeStringList(pending_command_manager.ColumnText(2));
        const auto completed = DecodeStringList(pending_command_manager.ColumnText(3));
        const auto response_ids = DecodeStringList(pending_command_manager.ColumnText(4));
        std::optional<mqtt::CommandResponsePayload> failure;
        const std::string failure_text = pending_command_manager.ColumnText(5);
        if (!failure_text.empty()) {
            const auto failure_message = mqtt::DeserializeMessage(failure_text);
            const auto* payload = failure_message.IsSuccess()
                                      ? mqtt::GetPayload<mqtt::CommandResponsePayload>(failure_message.value)
                                      : nullptr;
            if (payload == nullptr)
                return { DatabaseStatusCode::kSqlError, "stored command failure is invalid" };
            failure = *payload;
        }
        if (!message.IsSuccess() || !expected || !completed || !response_ids ||
            pending_command_manager.ColumnInt64(6) < 0) {
            return { DatabaseStatusCode::kSqlError, "stored command manager pending row is invalid" };
        }
        restored.command_manager.pending.push_back({ .request_id = pending_command_manager.ColumnText(0),
                                                     .original_message = message.value,
                                                     .expected_devices = *expected,
                                                     .completed_devices = *completed,
                                                     .response_message_ids = *response_ids,
                                                     .failure = failure,
                                                     .deadline_at_ms = pending_command_manager.ColumnInt64(6) });
    }
    if (!status.ok())
        return status;
    Statement completed_command_manager;
    status = database_.Prepare("SELECT request_id FROM command_manager_completed ORDER BY sequence",
                               completed_command_manager);
    if (!status.ok())
        return status;
    while ((status = completed_command_manager.Step(row)).ok() && row)
        restored.command_manager.completed_requests.push_back(completed_command_manager.ColumnText(0));
    if (!status.ok())
        return status;
    Statement system_commands;
    status = database_.Prepare("SELECT request_id,command FROM pending_system_command", system_commands);
    if (!status.ok())
        return status;
    while ((status = system_commands.Step(row)).ok() && row) {
        const auto command = mqtt::ControlCommandFromString(system_commands.ColumnText(1));
        if (command == mqtt::ControlCommand::kUnknown)
            return { DatabaseStatusCode::kSqlError, "stored system command is invalid" };
        restored.pending_system_commands.emplace(system_commands.ColumnText(0), command);
    }
    if (!status.ok())
        return status;
    Statement processed_messages;
    status =
        database_.Prepare("SELECT message_id FROM process_processed_message ORDER BY sequence", processed_messages);
    if (!status.ok()) {
        return status;
    }
    while ((status = processed_messages.Step(row)).ok() && row) {
        const std::string message_id = processed_messages.ColumnText(0);
        if (message_id.empty()) {
            return { DatabaseStatusCode::kSqlError, "stored processed message ID is invalid" };
        }
        restored.processed_message_ids.push_back(message_id);
    }
    if (!status.ok()) {
        return status;
    }
    output = std::move(restored);
    return DatabaseStatus::Ok();
}

DatabaseStatus ProcessStateStore::Save(
    ProcessSystemState system_state, std::uint64_t message_sequence, const std::vector<WorkProcessSnapshot>& works,
    const std::unordered_map<std::string, GripperTarget>& gripper_targets,
    const std::vector<ProcessCommandIntent>& pending_commands, std::int64_t updated_at_ms,
    const std::vector<PendingMqttDelivery>& deliveries, const std::vector<std::string>& processed_message_ids,
    const CommandManagerSnapshot& command_manager,
    const std::unordered_map<std::string, mqtt::ControlCommand>& pending_system_commands) {
    return SaveSnapshot(system_state, message_sequence, works, gripper_targets, pending_commands, updated_at_ms,
                        deliveries, processed_message_ids, command_manager, pending_system_commands, false);
}

DatabaseStatus ProcessStateStore::CommitRecovery(std::uint64_t message_sequence, std::uint64_t command_message_sequence,
                                                 std::int64_t updated_at_ms,
                                                 const std::vector<PendingMqttDelivery>& terminal_deliveries) {
    return SaveSnapshot(ProcessSystemState::kStopped, message_sequence, {}, {}, {}, updated_at_ms, terminal_deliveries,
                        {},
                        CommandManagerSnapshot{
                            .pending = {},
                            .completed_requests = {},
                            .message_sequence = command_message_sequence,
                        },
                        {}, true);
}

DatabaseStatus ProcessStateStore::SaveSnapshot(
    ProcessSystemState system_state, std::uint64_t message_sequence, const std::vector<WorkProcessSnapshot>& works,
    const std::unordered_map<std::string, GripperTarget>& gripper_targets,
    const std::vector<ProcessCommandIntent>& pending_commands, std::int64_t updated_at_ms,
    const std::vector<PendingMqttDelivery>& deliveries, const std::vector<std::string>& processed_message_ids,
    const CommandManagerSnapshot& command_manager,
    const std::unordered_map<std::string, mqtt::ControlCommand>& pending_system_commands, bool replace_mqtt_outbox) {
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
    if (!(status = database_.Execute("DELETE FROM process_processed_message")).ok()) {
        return status;
    }
    Statement insert_processed_message;
    status = database_.Prepare("INSERT INTO process_processed_message(message_id,sequence) VALUES(?,?)",
                               insert_processed_message);
    if (!status.ok()) {
        return status;
    }
    std::unordered_set<std::string_view> unique_processed_message_ids;
    for (std::size_t sequence = 0; sequence < processed_message_ids.size(); ++sequence) {
        const auto& message_id = processed_message_ids[sequence];
        if (message_id.empty() || !unique_processed_message_ids.emplace(message_id).second) {
            return { DatabaseStatusCode::kInvalidArgument, "invalid processed message IDs" };
        }
        if (!(status = insert_processed_message.Bind(1, message_id)).ok() ||
            !(status = insert_processed_message.Bind(2, static_cast<std::int64_t>(sequence))).ok() ||
            !(status = insert_processed_message.Step(row)).ok() || !(status = insert_processed_message.Reset()).ok()) {
            return status;
        }
    }
    if (!(status = database_.Execute("DELETE FROM command_manager_pending")).ok() ||
        !(status = database_.Execute("DELETE FROM command_manager_completed")).ok() ||
        !(status = database_.Execute("DELETE FROM pending_system_command")).ok())
        return status;
    Statement command_runtime;
    status = database_.Prepare(
        "INSERT INTO command_manager_runtime(id,message_sequence) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET "
        "message_sequence=excluded.message_sequence",
        command_runtime);
    if (!status.ok() ||
        !(status = command_runtime.Bind(1, static_cast<std::int64_t>(command_manager.message_sequence))).ok() ||
        !(status = command_runtime.Step(row)).ok())
        return status;
    Statement insert_pending_manager;
    status = database_.Prepare(
        "INSERT INTO "
        "command_manager_pending(request_id,message_json,expected_devices_json,completed_devices_json,response_message_"
        "ids_json,failure_json,deadline_at_ms) VALUES(?,?,?,?,?,?,?)",
        insert_pending_manager);
    if (!status.ok())
        return status;
    for (const auto& pending : command_manager.pending) {
        const auto encoded = mqtt::SerializeMessage(pending.original_message);
        std::string failure;
        if (pending.failure) {
            mqtt::MqttMessage failure_message{ .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                                               .message_id = "STORED-" + pending.request_id,
                                               .message_type = mqtt::MessageType::kCommandResponse,
                                               .source_id = "central-server",
                                               .timestamp = "1970-01-01T00:00:00Z",
                                               .data = *pending.failure };
            const auto encoded_failure = mqtt::SerializeMessage(failure_message);
            if (!encoded_failure.IsSuccess())
                return { DatabaseStatusCode::kInvalidArgument, "invalid command failure" };
            failure = encoded_failure.payload;
        }
        if (pending.request_id.empty() || !encoded.IsSuccess() || pending.deadline_at_ms < 0) {
            return { DatabaseStatusCode::kInvalidArgument,
                     "invalid command manager pending snapshot: " + encoded.status.message };
        }
        if (!(status = insert_pending_manager.Bind(1, pending.request_id)).ok() ||
            !(status = insert_pending_manager.Bind(2, encoded.payload)).ok() ||
            !(status = insert_pending_manager.Bind(3, EncodeStringList(pending.expected_devices))).ok() ||
            !(status = insert_pending_manager.Bind(4, EncodeStringList(pending.completed_devices))).ok() ||
            !(status = insert_pending_manager.Bind(5, EncodeStringList(pending.response_message_ids))).ok() ||
            !(status = insert_pending_manager.Bind(6, failure)).ok() ||
            !(status = insert_pending_manager.Bind(7, pending.deadline_at_ms)).ok() ||
            !(status = insert_pending_manager.Step(row)).ok() || !(status = insert_pending_manager.Reset()).ok())
            return status;
    }
    Statement insert_completed_manager;
    status = database_.Prepare("INSERT INTO command_manager_completed(request_id,sequence) VALUES(?,?)",
                               insert_completed_manager);
    if (!status.ok())
        return status;
    for (std::size_t sequence = 0; sequence < command_manager.completed_requests.size(); ++sequence) {
        if (command_manager.completed_requests[sequence].empty()) {
            return { DatabaseStatusCode::kInvalidArgument, "invalid completed command request ID" };
        }
        if (!(status = insert_completed_manager.Bind(1, command_manager.completed_requests[sequence])).ok() ||
            !(status = insert_completed_manager.Bind(2, static_cast<std::int64_t>(sequence))).ok() ||
            !(status = insert_completed_manager.Step(row)).ok() || !(status = insert_completed_manager.Reset()).ok())
            return status;
    }
    Statement insert_system;
    status = database_.Prepare("INSERT INTO pending_system_command(request_id,command) VALUES(?,?)", insert_system);
    if (!status.ok())
        return status;
    for (const auto& [request_id, command] : pending_system_commands) {
        if (request_id.empty() || command == mqtt::ControlCommand::kUnknown) {
            return { DatabaseStatusCode::kInvalidArgument, "invalid pending system command" };
        }
        if (!(status = insert_system.Bind(1, request_id)).ok() ||
            !(status = insert_system.Bind(2, mqtt::ToString(command))).ok() ||
            !(status = insert_system.Step(row)).ok() || !(status = insert_system.Reset()).ok())
            return status;
    }
    if (replace_mqtt_outbox && !(status = database_.Execute("DELETE FROM process_mqtt_outbox")).ok()) {
        return status;
    }
    Statement insert_delivery;
    if (!deliveries.empty()) {
        status = database_.Prepare(
            "INSERT INTO process_mqtt_outbox(topic,message_id,message_json,created_at_ms) VALUES(?,?,?,?) "
            "ON CONFLICT(topic,message_id) DO NOTHING",
            insert_delivery);
        if (!status.ok()) {
            return status;
        }
    }
    for (const auto& delivery : deliveries) {
        const auto encoded = mqtt::SerializeMessage(delivery.message);
        if (delivery.topic.empty() || !encoded.IsSuccess() ||
            !mqtt::ValidateTopicMessage(delivery.topic, delivery.message).IsSuccess()) {
            return { DatabaseStatusCode::kInvalidArgument, "invalid MQTT delivery" };
        }
        if (!(status = insert_delivery.Bind(1, delivery.topic)).ok() ||
            !(status = insert_delivery.Bind(2, delivery.message.message_id)).ok() ||
            !(status = insert_delivery.Bind(3, encoded.payload)).ok() ||
            !(status = insert_delivery.Bind(4, updated_at_ms)).ok() || !(status = insert_delivery.Step(row)).ok() ||
            !(status = insert_delivery.Reset()).ok()) {
            return status;
        }
    }
    return transaction.Commit();
}

DatabaseStatus ProcessStateStore::LoadPendingMqttDeliveries(std::vector<PendingMqttDelivery>& output) {
    output.clear();
    Statement deliveries;
    auto status = database_.Prepare("SELECT topic,message_json FROM process_mqtt_outbox ORDER BY created_at_ms,rowid",
                                    deliveries);
    if (!status.ok()) {
        return status;
    }
    bool row = false;
    while ((status = deliveries.Step(row)).ok() && row) {
        const auto decoded = mqtt::DeserializeMessage(deliveries.ColumnText(1));
        if (!decoded.IsSuccess() ||
            !contracts::mqtt::ValidateTopicMessage(deliveries.ColumnText(0), decoded.value).IsSuccess()) {
            return { DatabaseStatusCode::kSqlError, "stored MQTT delivery is invalid" };
        }
        output.push_back({ .topic = deliveries.ColumnText(0), .message = decoded.value });
    }
    return status;
}

DatabaseStatus ProcessStateStore::EnqueueMqttDelivery(std::string_view topic, const mqtt::MqttMessage& message,
                                                      std::int64_t created_at_ms) {
    return EnqueueMqttDeliveries({ PendingMqttDelivery{ .topic = std::string(topic), .message = message } },
                                 created_at_ms);
}

DatabaseStatus ProcessStateStore::EnqueueMqttDeliveries(const std::vector<PendingMqttDelivery>& deliveries,
                                                        std::int64_t created_at_ms) {
    if (deliveries.empty() || created_at_ms < 0) {
        return { DatabaseStatusCode::kInvalidArgument, "invalid MQTT deliveries" };
    }
    Transaction transaction(database_);
    if (!transaction.status().ok()) {
        return transaction.status();
    }
    Statement insert;
    auto status = database_.Prepare(
        "INSERT INTO process_mqtt_outbox(topic,message_id,message_json,created_at_ms) VALUES(?,?,?,?) "
        "ON CONFLICT(topic,message_id) DO NOTHING",
        insert);
    if (!status.ok()) {
        return status;
    }
    bool row = false;
    for (const auto& delivery : deliveries) {
        const auto encoded = mqtt::SerializeMessage(delivery.message);
        if (delivery.topic.empty() || !encoded.IsSuccess() ||
            !mqtt::ValidateTopicMessage(delivery.topic, delivery.message).IsSuccess()) {
            return { DatabaseStatusCode::kInvalidArgument, "invalid MQTT delivery" };
        }
        if (!(status = insert.Bind(1, delivery.topic)).ok() ||
            !(status = insert.Bind(2, delivery.message.message_id)).ok() ||
            !(status = insert.Bind(3, encoded.payload)).ok() || !(status = insert.Bind(4, created_at_ms)).ok() ||
            !(status = insert.Step(row)).ok() || !(status = insert.Reset()).ok()) {
            return status;
        }
    }
    return transaction.Commit();
}

DatabaseStatus ProcessStateStore::RemoveMqttDelivery(std::string_view topic, std::string_view message_id) {
    Statement remove;
    auto status = database_.Prepare("DELETE FROM process_mqtt_outbox WHERE topic=? AND message_id=?", remove);
    if (!status.ok() || !(status = remove.Bind(1, topic)).ok() || !(status = remove.Bind(2, message_id)).ok()) {
        return status;
    }
    bool row = false;
    return remove.Step(row);
}

}  // namespace logistics::central_server
