#include "logistics/central_server/history_service.hpp"

#include "logistics/contracts/identifier.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {
namespace {

DatabaseStatus ReadEntries(Statement& statement, std::vector<HistoryEntry>& output) {
    output.clear();
    bool row = false;
    DatabaseStatus status;
    while ((status = statement.Step(row)).ok() && row) {
        output.push_back({
            .message_id = statement.ColumnText(0),
            .event_type = statement.ColumnText(1),
            .source_id = statement.ColumnText(2),
            .state = statement.ColumnText(3),
            .error_code = statement.ColumnText(4),
            .severity = statement.ColumnText(5),
            .message = statement.ColumnText(6),
            .details_json = statement.ColumnText(7),
            .occurred_at_ms = statement.ColumnInt64(8),
        });
    }
    return status;
}

DatabaseStatus ValidateLimit(std::size_t limit) {
    if (limit == 0 || limit > HistoryService::kMaximumLimit) {
        return { DatabaseStatusCode::kInvalidArgument, "history query limit must be between 1 and 500" };
    }
    return DatabaseStatus::Ok();
}

}  // namespace

DatabaseStatus HistoryService::FindByWorkId(std::string_view work_id, std::size_t limit,
                                            std::vector<HistoryEntry>& output) {
    output.clear();
    if (!contracts::IsValidUuid(work_id)) {
        return { DatabaseStatusCode::kInvalidArgument, "workId must be a UUID" };
    }
    auto status = ValidateLimit(limit);
    if (!status.ok()) {
        return status;
    }

    Statement statement;
    status = database_.Prepare(
        "SELECT message_id,event_type,source_id,COALESCE(process_state,''),'','','',details_json,occurred_at_ms "
        "FROM work_history WHERE work_id=? "
        "UNION ALL "
        "SELECT COALESCE(message_id,''),'ERROR_OCCURRED',device_id,'',error_code,severity,error_message,"
        "details_json,occurred_at_ms FROM error_log WHERE work_id=? "
        "ORDER BY occurred_at_ms DESC LIMIT ?",
        statement);
    if (!status.ok() || !(status = statement.Bind(1, work_id)).ok() || !(status = statement.Bind(2, work_id)).ok() ||
        !(status = statement.Bind(3, static_cast<std::int64_t>(limit))).ok()) {
        return status;
    }
    return ReadEntries(statement, output);
}

DatabaseStatus HistoryService::FindByDeviceId(std::string_view device_id, std::size_t limit,
                                              std::vector<HistoryEntry>& output) {
    output.clear();
    if (!contracts::mqtt::IsValidTopicLevel(device_id)) {
        return { DatabaseStatusCode::kInvalidArgument, "deviceId must be one MQTT topic level" };
    }
    auto status = ValidateLimit(limit);
    if (!status.ok()) {
        return status;
    }

    Statement statement;
    status = database_.Prepare(
        "SELECT COALESCE(message_id,''),COALESCE(message_type,'MQTT_EVENT'),COALESCE(source_id,''),"
        "processing_state,COALESCE(failure_reason,''),'','',payload_json,received_at_ms "
        "FROM mqtt_event_log WHERE source_id=? AND COALESCE(message_type,'')<>'ERROR_OCCURRED' "
        "UNION ALL "
        "SELECT COALESCE(message_id,''),'ERROR_OCCURRED',device_id,'',error_code,severity,error_message,"
        "details_json,occurred_at_ms FROM error_log WHERE device_id=? "
        "ORDER BY received_at_ms DESC LIMIT ?",
        statement);
    if (!status.ok() || !(status = statement.Bind(1, device_id)).ok() ||
        !(status = statement.Bind(2, device_id)).ok() ||
        !(status = statement.Bind(3, static_cast<std::int64_t>(limit))).ok()) {
        return status;
    }
    return ReadEntries(statement, output);
}

}  // namespace logistics::central_server
