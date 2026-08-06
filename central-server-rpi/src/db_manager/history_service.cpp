#include "logistics/central_server/history_service.hpp"

#include <charconv>
#include <optional>

#include "logistics/contracts/identifier.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {
namespace {

struct HistoryCursor final {
    std::int64_t occurred_at_ms{};
    int source_rank{};
    std::int64_t row_id{};
};

std::string EncodeCursor(const HistoryCursor& cursor) {
    return std::to_string(cursor.occurred_at_ms) + "." + std::to_string(cursor.source_rank) + "." +
           std::to_string(cursor.row_id);
}

template <typename Integer>
bool ParseInteger(std::string_view text, Integer& output) {
    if (text.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

DatabaseStatus ParseCursor(std::string_view value, std::optional<HistoryCursor>& output) {
    output.reset();
    if (value.empty()) {
        return DatabaseStatus::Ok();
    }
    const auto first_separator = value.find('.');
    const auto second_separator =
        first_separator == std::string_view::npos ? std::string_view::npos : value.find('.', first_separator + 1);
    if (first_separator == std::string_view::npos || second_separator == std::string_view::npos ||
        value.find('.', second_separator + 1) != std::string_view::npos) {
        return { DatabaseStatusCode::kInvalidArgument, "history cursor is invalid" };
    }

    HistoryCursor cursor;
    if (!ParseInteger(value.substr(0, first_separator), cursor.occurred_at_ms) ||
        !ParseInteger(value.substr(first_separator + 1, second_separator - first_separator - 1), cursor.source_rank) ||
        !ParseInteger(value.substr(second_separator + 1), cursor.row_id) || cursor.occurred_at_ms < 0 ||
        cursor.source_rank < 1 || cursor.source_rank > 2 || cursor.row_id <= 0) {
        return { DatabaseStatusCode::kInvalidArgument, "history cursor is invalid" };
    }
    output = cursor;
    return DatabaseStatus::Ok();
}

DatabaseStatus BindCursorAndLimit(Statement& statement, int first_index, const std::optional<HistoryCursor>& cursor,
                                  std::size_t limit) {
    const HistoryCursor value = cursor.value_or(HistoryCursor{});
    DatabaseStatus status;
    if (!(status = statement.Bind(first_index, cursor ? 1 : 0)).ok() ||
        !(status = statement.Bind(first_index + 1, value.occurred_at_ms)).ok() ||
        !(status = statement.Bind(first_index + 2, value.occurred_at_ms)).ok() ||
        !(status = statement.Bind(first_index + 3, value.source_rank)).ok() ||
        !(status = statement.Bind(first_index + 4, value.source_rank)).ok() ||
        !(status = statement.Bind(first_index + 5, value.row_id)).ok() ||
        !(status = statement.Bind(first_index + 6, static_cast<std::int64_t>(limit + 1))).ok()) {
        return status;
    }
    return DatabaseStatus::Ok();
}

DatabaseStatus ReadPage(Statement& statement, std::size_t limit, HistoryPage& output) {
    output = {};
    bool row = false;
    DatabaseStatus status;
    while ((status = statement.Step(row)).ok() && row) {
        const HistoryCursor cursor{ .occurred_at_ms = statement.ColumnInt64(8),
                                    .source_rank = statement.ColumnInt(9),
                                    .row_id = statement.ColumnInt64(10) };
        output.entries.push_back({
            .history_id = EncodeCursor(cursor),
            .message_id = statement.ColumnText(0),
            .event_type = statement.ColumnText(1),
            .source_id = statement.ColumnText(2),
            .state = statement.ColumnText(3),
            .error_code = statement.ColumnText(4),
            .severity = statement.ColumnText(5),
            .message = statement.ColumnText(6),
            .details_json = statement.ColumnText(7),
            .occurred_at_ms = cursor.occurred_at_ms,
        });
    }
    if (!status.ok()) {
        return status;
    }
    if (output.entries.size() > limit) {
        output.entries.resize(limit);
        output.next_cursor = output.entries.back().history_id;
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

DatabaseStatus HistoryService::FindAll(std::size_t limit, std::string_view cursor, HistoryPage& output) {
    output = {};
    auto status = ValidateLimit(limit);
    if (!status.ok()) {
        return status;
    }
    std::optional<HistoryCursor> parsed_cursor;
    if (!(status = ParseCursor(cursor, parsed_cursor)).ok()) {
        return status;
    }

    Statement statement;
    status = database_.Prepare(
        "SELECT message_id,event_type,source_id,state,error_code,severity,message,details_json,occurred_at_ms,"
        "source_rank,row_id FROM ("
        "SELECT COALESCE(message_id,'') AS message_id,COALESCE(message_type,'MQTT_EVENT') AS event_type,"
        "COALESCE(source_id,'') AS source_id,processing_state AS state,COALESCE(failure_reason,'') AS error_code,"
        "'' AS severity,'' AS message,payload_json AS details_json,received_at_ms AS occurred_at_ms,"
        "2 AS source_rank,id AS row_id FROM mqtt_event_log WHERE COALESCE(message_type,'')<>'ERROR_OCCURRED' "
        "UNION ALL "
        "SELECT COALESCE(message_id,''),'ERROR_OCCURRED',device_id,'',error_code,severity,error_message,details_json,"
        "occurred_at_ms,1,id FROM error_log) AS history "
        "WHERE (?=0 OR occurred_at_ms<? OR (occurred_at_ms=? AND "
        "(source_rank<? OR (source_rank=? AND row_id<?)))) "
        "ORDER BY occurred_at_ms DESC,source_rank DESC,row_id DESC LIMIT ?",
        statement);
    if (!status.ok() || !(status = BindCursorAndLimit(statement, 1, parsed_cursor, limit)).ok()) {
        return status;
    }
    return ReadPage(statement, limit, output);
}

DatabaseStatus HistoryService::FindByWorkId(std::string_view work_id, std::size_t limit, std::string_view cursor,
                                            HistoryPage& output) {
    output = {};
    if (!contracts::IsValidUuid(work_id)) {
        return { DatabaseStatusCode::kInvalidArgument, "workId must be a UUID" };
    }
    auto status = ValidateLimit(limit);
    if (!status.ok()) {
        return status;
    }
    std::optional<HistoryCursor> parsed_cursor;
    if (!(status = ParseCursor(cursor, parsed_cursor)).ok()) {
        return status;
    }

    Statement statement;
    status = database_.Prepare(
        "SELECT message_id,event_type,source_id,state,error_code,severity,message,details_json,occurred_at_ms,"
        "source_rank,row_id FROM ("
        "SELECT message_id,event_type,source_id,COALESCE(process_state,'') AS state,'' AS error_code,'' AS severity,"
        "'' AS message,details_json,occurred_at_ms,2 AS source_rank,id AS row_id "
        "FROM work_history WHERE work_id=? "
        "UNION ALL "
        "SELECT COALESCE(message_id,''),'ERROR_OCCURRED',device_id,'',error_code,severity,error_message,details_json,"
        "occurred_at_ms,1,id FROM error_log WHERE work_id=?) AS history "
        "WHERE (?=0 OR occurred_at_ms<? OR (occurred_at_ms=? AND "
        "(source_rank<? OR (source_rank=? AND row_id<?)))) "
        "ORDER BY occurred_at_ms DESC,source_rank DESC,row_id DESC LIMIT ?",
        statement);
    if (!status.ok() || !(status = statement.Bind(1, work_id)).ok() || !(status = statement.Bind(2, work_id)).ok() ||
        !(status = BindCursorAndLimit(statement, 3, parsed_cursor, limit)).ok()) {
        return status;
    }
    return ReadPage(statement, limit, output);
}

DatabaseStatus HistoryService::FindByDeviceId(std::string_view device_id, std::size_t limit, std::string_view cursor,
                                              HistoryPage& output) {
    output = {};
    if (!contracts::mqtt::IsValidTopicLevel(device_id)) {
        return { DatabaseStatusCode::kInvalidArgument, "deviceId must be one MQTT topic level" };
    }
    auto status = ValidateLimit(limit);
    if (!status.ok()) {
        return status;
    }
    std::optional<HistoryCursor> parsed_cursor;
    if (!(status = ParseCursor(cursor, parsed_cursor)).ok()) {
        return status;
    }

    Statement statement;
    status = database_.Prepare(
        "SELECT message_id,event_type,source_id,state,error_code,severity,message,details_json,occurred_at_ms,"
        "source_rank,row_id FROM ("
        "SELECT COALESCE(message_id,'') AS message_id,COALESCE(message_type,'MQTT_EVENT') AS event_type,"
        "COALESCE(source_id,'') AS source_id,processing_state AS state,COALESCE(failure_reason,'') AS error_code,"
        "'' AS severity,'' AS message,payload_json AS details_json,received_at_ms AS occurred_at_ms,"
        "2 AS source_rank,id AS row_id "
        "FROM mqtt_event_log WHERE source_id=? AND COALESCE(message_type,'')<>'ERROR_OCCURRED' "
        "UNION ALL "
        "SELECT COALESCE(message_id,''),'ERROR_OCCURRED',device_id,'',error_code,severity,error_message,details_json,"
        "occurred_at_ms,1,id FROM error_log WHERE device_id=?) AS history "
        "WHERE (?=0 OR occurred_at_ms<? OR (occurred_at_ms=? AND "
        "(source_rank<? OR (source_rank=? AND row_id<?)))) "
        "ORDER BY occurred_at_ms DESC,source_rank DESC,row_id DESC LIMIT ?",
        statement);
    if (!status.ok() || !(status = statement.Bind(1, device_id)).ok() ||
        !(status = statement.Bind(2, device_id)).ok() ||
        !(status = BindCursorAndLimit(statement, 3, parsed_cursor, limit)).ok()) {
        return status;
    }
    return ReadPage(statement, limit, output);
}

}  // namespace logistics::central_server
