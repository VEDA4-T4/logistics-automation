#include "logistics/control_center/mqtt_message_validator.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

MqttValidationResult Invalid(QString error) {
    return { .is_valid = false, .error = std::move(error), .envelope = {} };
}

bool IsAllowedInboundTopic(const mqtt::ParsedTopic& parsed, const std::string& client_id) {
    switch (parsed.kind) {
        case mqtt::TopicKind::kQtResponse:
        case mqtt::TopicKind::kQtStatus:
        case mqtt::TopicKind::kQtEvent:
        case mqtt::TopicKind::kQtError:
            return parsed.endpoint_id == client_id;
        case mqtt::TopicKind::kServerStatus:
        case mqtt::TopicKind::kServerHeartbeat:
            return true;
        default:
            return false;
    }
}

bool IsValidTimestamp(const QString& value) {
    static const QRegularExpression timezone_pattern(QStringLiteral("(?:Z|[+-]\\d{2}:\\d{2})$"));
    if (!timezone_pattern.match(value).hasMatch()) {
        return false;
    }

    auto timestamp = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!timestamp.isValid()) {
        timestamp = QDateTime::fromString(value, Qt::ISODate);
    }
    return timestamp.isValid();
}

}  // namespace

MqttValidationResult MqttMessageValidator::Validate(const QString& topic, const QByteArray& payload,
                                                    const QString& client_id) {
    const auto topic_text = topic.toStdString();
    const auto client_id_text = client_id.toStdString();
    const auto parsed_topic = mqtt::ParseTopic(topic_text);
    if (!parsed_topic.IsValid() || !IsAllowedInboundTopic(parsed_topic, client_id_text)) {
        return Invalid(QStringLiteral("허용되지 않은 MQTT 토픽입니다: %1").arg(topic));
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(payload, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return Invalid(QStringLiteral("JSON 형식이 올바르지 않습니다: %1").arg(parse_error.errorString()));
    }

    const auto envelope = document.object();
    const auto protocol_version = envelope.value(QString::fromLatin1(mqtt::kProtocolVersionField));
    const auto message_id = envelope.value(QString::fromLatin1(mqtt::kMessageIdField));
    const auto message_type = envelope.value(QString::fromLatin1(mqtt::kMessageTypeField));
    const auto source_id = envelope.value(QString::fromLatin1(mqtt::kSourceIdField));
    const auto timestamp = envelope.value(QString::fromLatin1(mqtt::kTimestampField));
    const auto data = envelope.value(QString::fromLatin1(mqtt::kDataField));

    if (!protocol_version.isString() ||
        protocol_version.toString() != QString::fromLatin1(mqtt::kCurrentProtocolVersion)) {
        return Invalid(QStringLiteral("지원하지 않는 protocolVersion입니다."));
    }
    if (!message_id.isString() || !mqtt::IsValidTopicLevel(message_id.toString().toStdString())) {
        return Invalid(QStringLiteral("messageId가 누락되었거나 잘못되었습니다."));
    }
    if (!message_type.isString() ||
        mqtt::MessageTypeFromString(message_type.toString().toStdString()) == mqtt::MessageType::kUnknown) {
        return Invalid(QStringLiteral("messageType이 누락되었거나 잘못되었습니다."));
    }
    if (!source_id.isString() || !mqtt::IsValidTopicLevel(source_id.toString().toStdString())) {
        return Invalid(QStringLiteral("sourceId가 누락되었거나 잘못되었습니다."));
    }
    if (!timestamp.isString() || !IsValidTimestamp(timestamp.toString())) {
        return Invalid(QStringLiteral("timestamp는 timezone이 포함된 ISO 8601 형식이어야 합니다."));
    }
    if (!data.isObject()) {
        return Invalid(QStringLiteral("data가 누락되었거나 JSON object가 아닙니다."));
    }

    return { .is_valid = true, .error = {}, .envelope = envelope };
}

}  // namespace logistics::control_center
