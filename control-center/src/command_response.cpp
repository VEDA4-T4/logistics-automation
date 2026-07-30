#include "logistics/control_center/command_response.hpp"

#include <QJsonValue>
#include <string>
#include <utility>

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

CommandResponse Invalid(QString error) {
    return { .is_valid = false,
             .error = std::move(error),
             .request_id = {},
             .command = mqtt::ControlCommand::kUnknown,
             .result = mqtt::CommandResult::kUnknown,
             .error_code = {},
             .message = {} };
}

}  // namespace

CommandResponse ParseCommandResponse(const QJsonObject& envelope) {
    const auto message_type = envelope.value(QString::fromLatin1(mqtt::kMessageTypeField));
    if (!message_type.isString() ||
        mqtt::MessageTypeFromString(message_type.toString().toStdString()) != mqtt::MessageType::kCommandResponse) {
        return Invalid(QStringLiteral("COMMAND_RESPONSE 메시지가 아닙니다."));
    }

    const auto data_value = envelope.value(QString::fromLatin1(mqtt::kDataField));
    if (!data_value.isObject()) {
        return Invalid(QStringLiteral("COMMAND_RESPONSE data가 JSON object가 아닙니다."));
    }

    const auto data = data_value.toObject();
    const auto request_id = data.value(QString::fromLatin1(mqtt::kRequestIdField));
    const auto command = data.value(QStringLiteral("command"));
    const auto result = data.value(QStringLiteral("result"));
    if (!request_id.isString() || !mqtt::IsValidTopicLevel(request_id.toString().toStdString())) {
        return Invalid(QStringLiteral("COMMAND_RESPONSE requestId가 누락되었거나 잘못되었습니다."));
    }
    if (!command.isString()) {
        return Invalid(QStringLiteral("COMMAND_RESPONSE command가 누락되었습니다."));
    }
    if (!result.isString()) {
        return Invalid(QStringLiteral("COMMAND_RESPONSE result가 누락되었습니다."));
    }

    const auto parsed_command = mqtt::ControlCommandFromString(command.toString().toStdString());
    const auto parsed_result = mqtt::CommandResultFromString(result.toString().toStdString());
    if (parsed_command == mqtt::ControlCommand::kUnknown) {
        return Invalid(QStringLiteral("알 수 없는 COMMAND_RESPONSE command입니다."));
    }
    if (parsed_result == mqtt::CommandResult::kUnknown) {
        return Invalid(QStringLiteral("알 수 없는 COMMAND_RESPONSE result입니다."));
    }

    const auto error_code = data.value(QStringLiteral("errorCode"));
    const auto message = data.value(QStringLiteral("message"));
    if (!error_code.isUndefined() && !error_code.isNull() && !error_code.isString()) {
        return Invalid(QStringLiteral("COMMAND_RESPONSE errorCode는 문자열 또는 null이어야 합니다."));
    }
    if (!message.isUndefined() && !message.isNull() && !message.isString()) {
        return Invalid(QStringLiteral("COMMAND_RESPONSE message는 문자열 또는 null이어야 합니다."));
    }

    return { .is_valid = true,
             .error = {},
             .request_id = request_id.toString(),
             .command = parsed_command,
             .result = parsed_result,
             .error_code = error_code.isString() ? error_code.toString() : QString{},
             .message = message.isString() ? message.toString() : QString{} };
}

}  // namespace logistics::control_center
