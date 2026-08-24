#include "logistics/control_center/operational_log_state.hpp"

#include <QJsonObject>
#include <cassert>

namespace {

QJsonObject Envelope(const QString& id, const QString& type, const QString& source, QJsonObject data,
                     const QString& timestamp = QStringLiteral("2026-07-23T02:00:00.000Z")) {
    return {
        { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
        { QStringLiteral("messageId"), id },
        { QStringLiteral("messageType"), type },
        { QStringLiteral("sourceId"), source },
        { QStringLiteral("timestamp"), timestamp },
        { QStringLiteral("data"), data },
    };
}

}  // namespace

int main() {
    using logistics::control_center::OperationalLogFilter;
    using logistics::control_center::OperationalLogSeverity;
    using logistics::control_center::OperationalLogState;

    OperationalLogState state;
    auto result = state.applyEnvelope(
        QStringLiteral("qt/control-center/error"),
        Envelope(QStringLiteral("ERROR-1"), QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-SORTING-01"),
                 { { QStringLiteral("jobId"), QStringLiteral("WORK-1") },
                   { QStringLiteral("errorCode"), QStringLiteral("SENSOR_TIMEOUT") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                   { QStringLiteral("message"), QStringLiteral("분류 센서 응답 없음") } }));
    assert(result.handled && result.applied && result.error.isEmpty());
    assert(state.entries().size() == 1);
    assert(state.entries().front().severity == OperationalLogSeverity::Error);
    assert(state.entries().front().device_id == QStringLiteral("PI-SORTING-01"));
    assert(state.entries().front().occurred_at.isValid());
    assert(state.activeAlertCount() == 1);

    result =
        state.applyEnvelope(QStringLiteral("qt/control-center/event"),
                            Envelope(QStringLiteral("VISION-DUPLICATE-1"), QStringLiteral("WORK_COMPLETED"),
                                     QStringLiteral("central-server"),
                                     { { QStringLiteral("workId"), QStringLiteral("WORK-DUPLICATE") },
                                       { QStringLiteral("result"), QStringLiteral("SKIPPED") },
                                       { QStringLiteral("message"), QStringLiteral("duplicate barcode skipped") } }));
    assert(result.handled && !result.applied && result.error.isEmpty());
    assert(state.entries().size() == 1);
    assert(state.activeAlertCount() == 1);

    result = state.applyEnvelope(
        QStringLiteral("qt/control-center/error"),
        Envelope(QStringLiteral("ERROR-1"), QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-SORTING-01"),
                 { { QStringLiteral("errorCode"), QStringLiteral("SENSOR_TIMEOUT") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                   { QStringLiteral("message"), QStringLiteral("중복") } }));
    assert(result.handled && !result.applied);
    assert(state.entries().size() == 1);

    result = state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("STATUS-1"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("status"), QStringLiteral("OFFLINE") },
                   { QStringLiteral("currentState"), QStringLiteral("DISCONNECTED") } },
                 QStringLiteral("2026-07-23T02:00:01.000Z")));
    assert(result.applied);
    assert(state.entries().front().category == QStringLiteral("통신 장애"));
    assert(state.entries().front().severity == OperationalLogSeverity::Error);

    result = state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("STATUS-2"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("status"), QStringLiteral("ONLINE") },
                   { QStringLiteral("currentState"), QStringLiteral("RUNNING") } }));
    assert(result.handled && !result.applied && result.error.isEmpty());

    OperationalLogState uart_status_state;
    result = uart_status_state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("UART-STATUS-1"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("status"), QStringLiteral("UART_ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("UART_DISCONNECTED") },
                   { QStringLiteral("errorCode"), QStringLiteral("ERR-UART-DISCONNECTED") } }));
    assert(result.handled && result.applied && result.error.isEmpty());
    assert(uart_status_state.entries().size() == 1);

    result = uart_status_state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("UART-HEARTBEAT-1"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("status"), QStringLiteral("ONLINE") },
                   { QStringLiteral("currentState"), QStringLiteral("UART_DISCONNECTED") },
                   { QStringLiteral("errorCode"), QStringLiteral("ERR-UART-DISCONNECTED") } }));
    assert(result.handled && !result.applied && result.error.isEmpty());
    assert(uart_status_state.entries().size() == 1);

    result = uart_status_state.applyEnvelope(
        QStringLiteral("qt/control-center/error"),
        Envelope(QStringLiteral("UART-TIMEOUT-ERROR-1"), QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("errorCode"), QStringLiteral("ERR-UART-TIMEOUT") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("FAULT") },
                   { QStringLiteral("message"), QStringLiteral("UART timeout") } }));
    assert(result.handled && result.applied && result.error.isEmpty());
    assert(uart_status_state.entries().size() == 2);

    result = uart_status_state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("UART-TIMEOUT-STATUS-1"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("status"), QStringLiteral("ONLINE") },
                   { QStringLiteral("currentState"), QStringLiteral("FAULT") },
                   { QStringLiteral("errorCode"), QStringLiteral("ERR-UART-TIMEOUT") } }));
    assert(result.handled && !result.applied && result.error.isEmpty());
    assert(uart_status_state.entries().size() == 2);

    result = uart_status_state.applyEnvelope(
        QStringLiteral("qt/control-center/error"),
        Envelope(QStringLiteral("UART-TIMEOUT-ERROR-2"), QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("errorCode"), QStringLiteral("ERR-UART-TIMEOUT") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("FAULT") },
                   { QStringLiteral("message"), QStringLiteral("duplicate UART timeout") } }));
    assert(result.handled && !result.applied && result.error.isEmpty());
    assert(uart_status_state.entries().size() == 2);

    result = uart_status_state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("UART-RECOVERED-1"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("status"), QStringLiteral("ONLINE") },
                   { QStringLiteral("currentState"), QStringLiteral("IDLE") } }));
    assert(result.handled && !result.applied && result.error.isEmpty());

    result = uart_status_state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("UART-STATUS-2"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-LT-01"),
                 { { QStringLiteral("status"), QStringLiteral("UART_ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("UART_DISCONNECTED") },
                   { QStringLiteral("errorCode"), QStringLiteral("ERR-UART-DISCONNECTED") } }));
    assert(result.handled && result.applied && result.error.isEmpty());
    assert(uart_status_state.entries().size() == 3);

    result = state.applyEnvelope(
        QStringLiteral("qt/control-center/error"),
        Envelope(QStringLiteral("STALE-ERROR"), QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-SORTING-01"),
                 { { QStringLiteral("errorCode"), QStringLiteral("ERR-HEALTH-SENSOR-STALE") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("CONTROLLER_HEALTH") },
                   { QStringLiteral("message"), QStringLiteral("sensor stale") } }));
    assert(result.applied);
    assert(state.entries().front().severity == OperationalLogSeverity::Warning);
    assert(state.entries().front().category == QStringLiteral("센서 경고"));
    assert(state.activeAlertCount() == 2);

    result = state.applyEnvelope(
        QStringLiteral("qt/control-center/status"),
        Envelope(QStringLiteral("STALE-STATUS"), QStringLiteral("DEVICE_STATUS"), QStringLiteral("PI-SORTING-01"),
                 { { QStringLiteral("status"), QStringLiteral("UART_ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("CONTROLLER_HEALTH") },
                   { QStringLiteral("errorCode"), QStringLiteral("ERR_HEALTH_SENSOR_STALE") } }));
    assert(result.applied);
    assert(state.entries().front().severity == OperationalLogSeverity::Warning);
    assert(state.entries().front().category == QStringLiteral("센서 경고"));
    assert(state.activeAlertCount() == 2);

    result = state.applyEnvelope(
        QStringLiteral("qt/control-center/event"),
        Envelope(QStringLiteral("BARCODE-1"), QStringLiteral("BARCODE_DETECTED"), QStringLiteral("PI-VISION-01"),
                 { { QStringLiteral("recognitionStatus"), QStringLiteral("FAILED") },
                   { QStringLiteral("message"), QStringLiteral("바코드 인식 실패") } }));
    assert(result.applied);
    assert(state.entries().front().severity == OperationalLogSeverity::Warning);

    result = state.applyEnvelope(
        QStringLiteral("qt/control-center/error"),
        Envelope(QStringLiteral("BAD-SEVERITY"), QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-INPUT-01"),
                 { { QStringLiteral("errorCode"), QStringLiteral("BAD") },
                   { QStringLiteral("errorLevel"), QStringLiteral("URGENT") },
                   { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                   { QStringLiteral("message"), QStringLiteral("잘못된 심각도") } }));
    assert(result.handled && !result.applied && !result.error.isEmpty());

    OperationalLogFilter filter{ .filter_by_severity = true,
                                 .severity = OperationalLogSeverity::Error,
                                 .query = QStringLiteral("LT-01"),
                                 .unacknowledged_only = true };
    const auto filtered = state.filteredEntries(filter);
    assert(filtered.size() == 1);
    assert(filtered.front().id == QStringLiteral("STATUS-1"));
    assert(state.acknowledge(QStringLiteral("STATUS-1")));
    assert(state.filteredEntries(filter).isEmpty());
    assert(!state.acknowledge(QStringLiteral("STATUS-1")));

    state.appendLocal(OperationalLogSeverity::Critical, QStringLiteral("control-center"), QStringLiteral("통신 장애"),
                      QStringLiteral("MQTT_AUTH_ERROR"), QStringLiteral("인증 실패"),
                      QDateTime::fromString(QStringLiteral("2026-07-23T02:00:02.000Z"), Qt::ISODateWithMs));
    assert(state.entries().front().severity == OperationalLogSeverity::Critical);
    assert(state.activeAlertCount() == 2);
    assert(state.acknowledgeAllAlerts() == 2);
    assert(state.activeAlertCount() == 0);
    assert(state.acknowledgeAllAlerts() == 0);

    const auto older_timestamp = QDateTime::fromString(QStringLiteral("2026-07-01T00:00:00.000Z"), Qt::ISODateWithMs);
    const logistics::control_center::OperationalLogEntry older_entry{
        .id = QStringLiteral("HISTORY-1"),
        .occurred_at = older_timestamp,
        .severity = OperationalLogSeverity::Info,
        .device_id = QStringLiteral("PI-INPUT-01"),
        .category = QStringLiteral("서버 이력"),
        .code = QStringLiteral("DEVICE_STATUS"),
        .message = QStringLiteral("과거 장치 상태"),
        .topic = QStringLiteral("server-history"),
        .acknowledged = false,
    };
    const auto inserted_history = state.appendOlderEntries({ older_entry, older_entry });
    assert(inserted_history.size() == 1);
    assert(state.entries().back().id == QStringLiteral("HISTORY-1"));
    assert(state.appendOlderEntries({ older_entry }).isEmpty());

    for (qsizetype index = 0; index < OperationalLogState::kDefaultMaximumEntries + 10; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("test"), QStringLiteral("테스트"),
                          QStringLiteral("ENTRY"), QString::number(index));
    }
    assert(state.entries().size() == OperationalLogState::kDefaultMaximumEntries);
    assert(state.maximumEntries() == OperationalLogState::kDefaultMaximumEntries);

    OperationalLogState overflowing_entries;
    for (qsizetype index = 0; index < OperationalLogState::kDefaultMaximumEntries + 10; ++index) {
        const auto severity = static_cast<OperationalLogSeverity>(index % 4);
        overflowing_entries.appendLocal(severity, QStringLiteral("PI-LOAD-01"), QStringLiteral("부하 로그"),
                                        QStringLiteral("OVERFLOW_ENTRY"), QString::number(index));
    }
    assert(overflowing_entries.entries().size() == OperationalLogState::kDefaultMaximumEntries);
    assert(overflowing_entries.unacknowledgedCount() == OperationalLogState::kDefaultMaximumEntries);
    assert(overflowing_entries.activeAlertCount() == OperationalLogState::kDefaultMaximumEntries / 2);
    const auto retained_oldest_entry_id = overflowing_entries.entries().back().id;
    assert(overflowing_entries.acknowledge(retained_oldest_entry_id));
    assert(overflowing_entries.unacknowledgedCount() == OperationalLogState::kDefaultMaximumEntries - 1);
    assert(overflowing_entries.activeAlertCount() == OperationalLogState::kDefaultMaximumEntries / 2 - 1);
    assert(overflowing_entries.acknowledgeAllAlerts() == OperationalLogState::kDefaultMaximumEntries / 2 - 1);
    assert(overflowing_entries.unacknowledgedCount() == OperationalLogState::kDefaultMaximumEntries / 2);

    OperationalLogState bounded_history(3);
    bounded_history.appendLocal(OperationalLogSeverity::Info, QStringLiteral("test"), QStringLiteral("live"),
                                QStringLiteral("LIVE"), QStringLiteral("1"));
    bounded_history.appendLocal(OperationalLogSeverity::Info, QStringLiteral("test"), QStringLiteral("live"),
                                QStringLiteral("LIVE"), QStringLiteral("2"));
    auto second_older_entry = older_entry;
    second_older_entry.id = QStringLiteral("HISTORY-2");
    second_older_entry.occurred_at = older_timestamp.addMSecs(-1);
    const auto bounded_inserted = bounded_history.appendOlderEntries({ older_entry, second_older_entry });
    assert(bounded_inserted.size() == 1);
    assert(bounded_history.entries().size() == 3);
    assert(bounded_history.entries().back().id == QStringLiteral("HISTORY-1"));

    OperationalLogState mqtt_overflow;
    for (qsizetype index = 0; index < OperationalLogState::kDefaultMaximumEntries * 2 + 10; ++index) {
        const auto id = QStringLiteral("LOAD-%1").arg(index);
        const auto update =
            mqtt_overflow.applyEnvelope(QStringLiteral("qt/control-center/error"),
                                        Envelope(id, QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-LOAD-01"),
                                                 { { QStringLiteral("errorCode"), QStringLiteral("LOAD") },
                                                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                                                   { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                                                   { QStringLiteral("message"), QStringLiteral("load") } }));
        assert(update.applied);
    }
    assert(mqtt_overflow.entries().size() == OperationalLogState::kDefaultMaximumEntries);
    assert(mqtt_overflow.processedMessageIdCount() <= OperationalLogState::kDefaultMaximumEntries * 2);
    const auto duplicate_update = mqtt_overflow.applyEnvelope(
        QStringLiteral("qt/control-center/error"),
        Envelope(QStringLiteral("LOAD-1009"), QStringLiteral("ERROR_OCCURRED"), QStringLiteral("PI-LOAD-01"),
                 { { QStringLiteral("errorCode"), QStringLiteral("LOAD") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                   { QStringLiteral("message"), QStringLiteral("duplicate") } }));
    assert(duplicate_update.handled && !duplicate_update.applied);
    return 0;
}
