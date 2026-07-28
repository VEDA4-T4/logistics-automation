#include "logistics/control_center/command_response.hpp"

#include <QJsonObject>
#include <cassert>

int main() {
    namespace mqtt = logistics::contracts::mqtt;
    using logistics::control_center::ParseCommandResponse;

    const QJsonObject valid_response{
        { QStringLiteral("messageType"), QStringLiteral("COMMAND_RESPONSE") },
        { QStringLiteral("data"),
          QJsonObject{
              { QStringLiteral("requestId"), QStringLiteral("REQ-0001") },
              { QStringLiteral("command"), QStringLiteral("START") },
              { QStringLiteral("result"), QStringLiteral("SUCCESS") },
              { QStringLiteral("errorCode"), QJsonValue::Null },
              { QStringLiteral("message"), QStringLiteral("공정이 시작되었습니다.") },
          } },
    };

    const auto parsed = ParseCommandResponse(valid_response);
    assert(parsed.is_valid);
    assert(parsed.request_id == QStringLiteral("REQ-0001"));
    assert(parsed.command == mqtt::ControlCommand::kStart);
    assert(parsed.result == mqtt::CommandResult::kSuccess);
    assert(parsed.error_code.isEmpty());
    assert(parsed.message == QStringLiteral("공정이 시작되었습니다."));

    auto null_message = valid_response;
    auto null_message_data = null_message.value(QStringLiteral("data")).toObject();
    null_message_data.insert(QStringLiteral("message"), QJsonValue::Null);
    null_message.insert(QStringLiteral("data"), null_message_data);
    const auto parsed_null_message = ParseCommandResponse(null_message);
    assert(parsed_null_message.is_valid);
    assert(parsed_null_message.message.isEmpty());

    auto invalid_type = valid_response;
    invalid_type.insert(QStringLiteral("messageType"), QStringLiteral("DEVICE_STATUS"));
    assert(!ParseCommandResponse(invalid_type).is_valid);

    auto missing_request = valid_response;
    auto missing_request_data = missing_request.value(QStringLiteral("data")).toObject();
    missing_request_data.remove(QStringLiteral("requestId"));
    missing_request.insert(QStringLiteral("data"), missing_request_data);
    assert(!ParseCommandResponse(missing_request).is_valid);

    auto unknown_result = valid_response;
    auto unknown_result_data = unknown_result.value(QStringLiteral("data")).toObject();
    unknown_result_data.insert(QStringLiteral("result"), QStringLiteral("NOT_A_RESULT"));
    unknown_result.insert(QStringLiteral("data"), unknown_result_data);
    assert(!ParseCommandResponse(unknown_result).is_valid);

    auto invalid_error_code = valid_response;
    auto invalid_error_code_data = invalid_error_code.value(QStringLiteral("data")).toObject();
    invalid_error_code_data.insert(QStringLiteral("errorCode"), true);
    invalid_error_code.insert(QStringLiteral("data"), invalid_error_code_data);
    assert(!ParseCommandResponse(invalid_error_code).is_valid);

    return 0;
}
