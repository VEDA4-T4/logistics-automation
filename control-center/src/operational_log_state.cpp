#include "logistics/control_center/operational_log_state.hpp"

#include <QJsonObject>
#include <algorithm>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/control_center/operations_dashboard_state.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

QString StringValue(const QJsonObject& object, const char* key) {
    const auto value = object.value(QString::fromLatin1(key));
    return value.isString() ? value.toString().trimmed() : QString{};
}

QDateTime ParseTimestamp(const QJsonObject& envelope) {
    const auto value = envelope.value(QString::fromLatin1(mqtt::kTimestampField));
    if (!value.isString()) {
        return {};
    }
    const auto timestamp = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    return timestamp.isValid() ? timestamp.toUTC() : QDateTime{};
}

bool IsTrackedMessage(mqtt::MessageType type) {
    switch (type) {
        case mqtt::MessageType::kDeviceStatus:
        case mqtt::MessageType::kWorkCreated:
        case mqtt::MessageType::kWorkCompleted:
        case mqtt::MessageType::kBarcodeDetected:
        case mqtt::MessageType::kProductInfo:
        case mqtt::MessageType::kErrorOccurred:
        case mqtt::MessageType::kEmergencyStop:
        case mqtt::MessageType::kCommandResponse:
            return true;
        default:
            return false;
    }
}

OperationalLogSeverity SeverityFromText(const QString& value, bool& valid) {
    const auto normalized = value.trimmed().toUpper();
    valid = true;
    if (normalized == QStringLiteral("INFO"))
        return OperationalLogSeverity::Info;
    if (normalized == QStringLiteral("WARNING"))
        return OperationalLogSeverity::Warning;
    if (normalized == QStringLiteral("ERROR"))
        return OperationalLogSeverity::Error;
    if (normalized == QStringLiteral("CRITICAL"))
        return OperationalLogSeverity::Critical;
    valid = false;
    return OperationalLogSeverity::Info;
}

OperationalLogSeverity DeviceStatusSeverity(mqtt::ConnectionState state) {
    switch (state) {
        case mqtt::ConnectionState::kDelayed:
        case mqtt::ConnectionState::kReconnecting:
            return OperationalLogSeverity::Warning;
        case mqtt::ConnectionState::kOffline:
        case mqtt::ConnectionState::kRtspError:
        case mqtt::ConnectionState::kMqttError:
        case mqtt::ConnectionState::kUartError:
            return OperationalLogSeverity::Error;
        case mqtt::ConnectionState::kMqttAuthError:
        case mqtt::ConnectionState::kTlsError:
            return OperationalLogSeverity::Critical;
        case mqtt::ConnectionState::kOnline:
        case mqtt::ConnectionState::kUnknown:
            return OperationalLogSeverity::Info;
    }
    return OperationalLogSeverity::Info;
}

QString DeviceStatusMessage(mqtt::ConnectionState state) {
    switch (state) {
        case mqtt::ConnectionState::kDelayed:
            return QStringLiteral("장치 응답이 지연되고 있습니다.");
        case mqtt::ConnectionState::kOffline:
            return QStringLiteral("장치 연결이 끊겼습니다.");
        case mqtt::ConnectionState::kReconnecting:
            return QStringLiteral("장치가 재연결을 시도하고 있습니다.");
        case mqtt::ConnectionState::kRtspError:
            return QStringLiteral("RTSP 영상 연결 오류가 발생했습니다.");
        case mqtt::ConnectionState::kMqttError:
            return QStringLiteral("MQTT 통신 오류가 발생했습니다.");
        case mqtt::ConnectionState::kMqttAuthError:
            return QStringLiteral("MQTT 인증에 실패했습니다.");
        case mqtt::ConnectionState::kTlsError:
            return QStringLiteral("TLS 연결 오류가 발생했습니다.");
        case mqtt::ConnectionState::kUartError:
            return QStringLiteral("UART 통신 오류가 발생했습니다.");
        case mqtt::ConnectionState::kOnline:
        case mqtt::ConnectionState::kUnknown:
            return {};
    }
    return {};
}

bool ContainsQuery(const OperationalLogEntry& entry, const QString& query) {
    if (query.isEmpty()) {
        return true;
    }
    return entry.device_id.contains(query, Qt::CaseInsensitive) ||
           entry.category.contains(query, Qt::CaseInsensitive) || entry.code.contains(query, Qt::CaseInsensitive) ||
           entry.message.contains(query, Qt::CaseInsensitive);
}

}  // namespace

OperationalLogState::OperationalLogState(qsizetype maximum_entries) {
    setMaximumEntries(maximum_entries);
}

OperationalLogUpdateResult OperationalLogState::applyEnvelope(const QString& topic, const QJsonObject& envelope) {
    const auto type_value = envelope.value(QString::fromLatin1(mqtt::kMessageTypeField));
    if (!type_value.isString()) {
        return {};
    }
    const auto type = mqtt::MessageTypeFromString(type_value.toString().toStdString());
    if (!IsTrackedMessage(type)) {
        return {};
    }

    OperationalLogUpdateResult result{ .handled = true, .applied = false, .error = {} };
    const auto message_id = StringValue(envelope, mqtt::kMessageIdField.data());
    const auto source_id = StringValue(envelope, mqtt::kSourceIdField.data());
    const auto timestamp = ParseTimestamp(envelope);
    const auto data_value = envelope.value(QString::fromLatin1(mqtt::kDataField));
    if (!mqtt::IsValidTopicLevel(message_id.toStdString()) || !mqtt::IsValidTopicLevel(source_id.toStdString()) ||
        !timestamp.isValid() || !data_value.isObject()) {
        result.error = QStringLiteral("운영 로그 메시지 envelope가 올바르지 않습니다.");
        return result;
    }
    if (processed_message_ids_.contains(message_id)) {
        return result;
    }

    const auto data = data_value.toObject();
    OperationalLogEntry entry{ .id = message_id,
                               .occurred_at = timestamp,
                               .severity = OperationalLogSeverity::Info,
                               .device_id = source_id,
                               .category = QStringLiteral("운영"),
                               .code = type_value.toString(),
                               .message = {},
                               .topic = topic,
                               .acknowledged = false };
    bool should_append = true;

    switch (type) {
        case mqtt::MessageType::kErrorOccurred: {
            bool severity_is_valid = false;
            entry.severity = SeverityFromText(StringValue(data, "errorLevel"), severity_is_valid);
            entry.code = StringValue(data, "errorCode");
            entry.message = StringValue(data, "message");
            if (!severity_is_valid || entry.code.isEmpty() || entry.message.isEmpty()) {
                result.error = QStringLiteral("오류 로그의 심각도, 코드 또는 내용이 올바르지 않습니다.");
                return result;
            }
            if (IsSensorStaleErrorCode(entry.code)) {
                entry.severity = OperationalLogSeverity::Warning;
                entry.category = QStringLiteral("센서 경고");
            } else {
                entry.category = QStringLiteral("장치 오류");
            }

            const auto alert_key = entry.code.toUpper();
            auto& active_alerts = active_device_alerts_[source_id];
            if (active_alerts.contains(alert_key)) {
                should_append = false;
                break;
            }
            active_alerts.insert(alert_key);
            break;
        }
        case mqtt::MessageType::kEmergencyStop:
            entry.severity = OperationalLogSeverity::Critical;
            entry.category = QStringLiteral("비상정지");
            entry.code = QStringLiteral("EMERGENCY_STOP");
            entry.message = QStringLiteral("비상정지 명령이 발생했습니다.");
            break;
        case mqtt::MessageType::kDeviceStatus: {
            const auto status_text = StringValue(data, "status");
            const auto state = mqtt::ConnectionStateFromString(status_text.toStdString());
            const auto current_state = StringValue(data, "currentState").toUpper();
            const auto error_code = StringValue(data, "errorCode");
            const bool sensor_stale = IsSensorStaleErrorCode(error_code);
            const bool process_error =
                !sensor_stale && (current_state == QStringLiteral("ERROR") ||
                                  current_state.endsWith(QStringLiteral("_ERROR")) || !error_code.isEmpty());
            if (state == mqtt::ConnectionState::kUnknown) {
                result.error = QStringLiteral("장치 상태 로그의 status가 올바르지 않습니다.");
                return result;
            }
            if (state == mqtt::ConnectionState::kOnline && !process_error && !sensor_stale) {
                active_device_alerts_.remove(source_id);
                should_append = false;
                break;
            }

            const auto normalized_error_code = error_code.toUpper();
            const auto alert_key = !normalized_error_code.isEmpty()
                                       ? normalized_error_code
                                       : QStringLiteral("%1|%2").arg(status_text.toUpper(), current_state);
            auto& active_alerts = active_device_alerts_[source_id];
            if (active_alerts.contains(alert_key)) {
                should_append = false;
                break;
            }
            active_alerts.insert(alert_key);

            entry.severity = sensor_stale
                                 ? OperationalLogSeverity::Warning
                                 : (process_error ? OperationalLogSeverity::Error : DeviceStatusSeverity(state));
            entry.category = sensor_stale ? QStringLiteral("센서 경고")
                                          : (process_error ? QStringLiteral("공정 경고") : QStringLiteral("통신 장애"));
            entry.code = !error_code.isEmpty() ? error_code : status_text;
            entry.message =
                sensor_stale ? QStringLiteral("센서 유효 표본이 설정 시간 동안 갱신되지 않았습니다.")
                             : (process_error ? QStringLiteral("장치가 오류 상태를 보고했습니다: %1").arg(current_state)
                                              : DeviceStatusMessage(state));
            break;
        }
        case mqtt::MessageType::kWorkCreated:
            entry.category = QStringLiteral("작업");
            entry.code = QStringLiteral("WORK_CREATED");
            entry.message = QStringLiteral("작업이 시작되었습니다: %1").arg(StringValue(data, "workId"));
            break;
        case mqtt::MessageType::kWorkCompleted: {
            const auto result_text = StringValue(data, "result").toUpper();
            const bool succeeded = result_text == QStringLiteral("SUCCESS");
            entry.severity = succeeded ? OperationalLogSeverity::Info : OperationalLogSeverity::Error;
            entry.category = QStringLiteral("작업");
            entry.code = result_text.isEmpty() ? QStringLiteral("WORK_COMPLETED") : result_text;
            entry.message =
                succeeded ? QStringLiteral("작업이 정상 완료되었습니다: %1").arg(StringValue(data, "workId"))
                          : QStringLiteral("작업이 실패 또는 중단되었습니다: %1").arg(StringValue(data, "workId"));
            break;
        }
        case mqtt::MessageType::kBarcodeDetected:
        case mqtt::MessageType::kProductInfo: {
            const auto recognition_status = StringValue(data, "recognitionStatus").toUpper();
            if (recognition_status == QStringLiteral("SUCCESS")) {
                should_append = false;
                break;
            }
            entry.severity = OperationalLogSeverity::Warning;
            entry.category = QStringLiteral("인식 경고");
            entry.code = recognition_status.isEmpty() ? QStringLiteral("RECOGNITION_FAILED") : recognition_status;
            entry.message = StringValue(data, "message");
            if (entry.message.isEmpty()) {
                entry.message = QStringLiteral("상품 인식 결과를 확인해야 합니다.");
            }
            break;
        }
        case mqtt::MessageType::kCommandResponse: {
            const auto command = StringValue(data, "command");
            const auto command_result = StringValue(data, "result").toUpper();
            const bool failed = command_result == QStringLiteral("FAILED") ||
                                command_result == QStringLiteral("REJECTED") ||
                                command_result == QStringLiteral("TIMEOUT");
            entry.severity = failed ? OperationalLogSeverity::Error : OperationalLogSeverity::Info;
            entry.category = QStringLiteral("관제 명령");
            entry.code = StringValue(data, "errorCode");
            if (entry.code.isEmpty()) {
                entry.code = command_result;
            }
            entry.message = StringValue(data, "message");
            if (entry.message.isEmpty()) {
                entry.message = QStringLiteral("%1 명령 결과: %2").arg(command, command_result);
            }
            break;
        }
        default:
            should_append = false;
            break;
    }

    if (should_append) {
        rememberProcessedMessageId(message_id);
        append(std::move(entry));
        result.applied = true;
    }
    return result;
}

void OperationalLogState::appendLocal(OperationalLogSeverity severity, const QString& device_id,
                                      const QString& category, const QString& code, const QString& message,
                                      const QDateTime& occurred_at) {
    const auto timestamp = occurred_at.isValid() ? occurred_at.toUTC() : QDateTime::currentDateTimeUtc();
    append({ .id = QStringLiteral("LOCAL-%1-%2").arg(timestamp.toMSecsSinceEpoch()).arg(++local_sequence_),
             .occurred_at = timestamp,
             .severity = severity,
             .device_id = device_id,
             .category = category,
             .code = code,
             .message = message,
             .topic = QStringLiteral("local"),
             .acknowledged = false });
}

QList<OperationalLogEntry> OperationalLogState::appendOlderEntries(QList<OperationalLogEntry> entries) {
    QList<OperationalLogEntry> inserted;
    inserted.reserve(entries.size());
    for (auto& entry : entries) {
        if (entries_.size() >= maximum_entries_) {
            break;
        }
        if (entry.id.isEmpty() || !entry.occurred_at.isValid() || processed_message_ids_.contains(entry.id)) {
            continue;
        }
        const auto duplicate = std::find_if(entries_.cbegin(), entries_.cend(),
                                            [&entry](const auto& existing) { return existing.id == entry.id; });
        if (duplicate != entries_.cend()) {
            continue;
        }
        rememberProcessedMessageId(entry.id);
        entries_.append(entry);
        inserted.append(std::move(entry));
    }
    return inserted;
}

bool OperationalLogState::acknowledge(const QString& id) {
    for (auto& entry : entries_) {
        if (entry.id == id && !entry.acknowledged) {
            entry.acknowledged = true;
            return true;
        }
    }
    return false;
}

int OperationalLogState::acknowledgeAllAlerts() {
    int acknowledged_count = 0;
    for (auto& entry : entries_) {
        const bool is_alert =
            entry.severity == OperationalLogSeverity::Error || entry.severity == OperationalLogSeverity::Critical;
        if (is_alert && !entry.acknowledged) {
            entry.acknowledged = true;
            ++acknowledged_count;
        }
    }
    return acknowledged_count;
}

QList<OperationalLogEntry> OperationalLogState::filteredEntries(const OperationalLogFilter& filter) const {
    QList<OperationalLogEntry> filtered;
    for (const auto& entry : entries_) {
        if (filter.filter_by_severity && entry.severity != filter.severity)
            continue;
        if (filter.unacknowledged_only && entry.acknowledged)
            continue;
        if (!ContainsQuery(entry, filter.query.trimmed()))
            continue;
        filtered.append(entry);
    }
    return filtered;
}

const QList<OperationalLogEntry>& OperationalLogState::entries() const noexcept {
    return entries_;
}

int OperationalLogState::unacknowledgedCount() const noexcept {
    return static_cast<int>(
        std::count_if(entries_.cbegin(), entries_.cend(), [](const auto& entry) { return !entry.acknowledged; }));
}

int OperationalLogState::activeAlertCount() const noexcept {
    return static_cast<int>(std::count_if(entries_.cbegin(), entries_.cend(), [](const auto& entry) {
        return !entry.acknowledged &&
               (entry.severity == OperationalLogSeverity::Error || entry.severity == OperationalLogSeverity::Critical);
    }));
}

qsizetype OperationalLogState::maximumEntries() const noexcept {
    return maximum_entries_;
}

qsizetype OperationalLogState::processedMessageIdCount() const noexcept {
    return processed_message_ids_.size();
}

void OperationalLogState::setMaximumEntries(qsizetype maximum_entries) {
    maximum_entries_ = std::max<qsizetype>(1, maximum_entries);
    while (entries_.size() > maximum_entries_) {
        entries_.removeLast();
    }
    trimProcessedMessageIds();
}

void OperationalLogState::append(OperationalLogEntry entry) {
    entries_.prepend(std::move(entry));
    while (entries_.size() > maximum_entries_) {
        entries_.removeLast();
    }
}

void OperationalLogState::rememberProcessedMessageId(const QString& id) {
    if (id.isEmpty() || processed_message_ids_.contains(id)) {
        return;
    }
    processed_message_ids_.insert(id);
    processed_message_id_order_.enqueue(id);
    trimProcessedMessageIds();
}

void OperationalLogState::trimProcessedMessageIds() {
    const auto maximum_processed_ids = maximum_entries_ * 2;
    while (processed_message_id_order_.size() > maximum_processed_ids) {
        processed_message_ids_.remove(processed_message_id_order_.dequeue());
    }
}

QString OperationalSeverityLabel(OperationalLogSeverity severity) {
    switch (severity) {
        case OperationalLogSeverity::Info:
            return QStringLiteral("정보");
        case OperationalLogSeverity::Warning:
            return QStringLiteral("경고");
        case OperationalLogSeverity::Error:
            return QStringLiteral("오류");
        case OperationalLogSeverity::Critical:
            return QStringLiteral("심각");
    }
    return {};
}

}  // namespace logistics::control_center
