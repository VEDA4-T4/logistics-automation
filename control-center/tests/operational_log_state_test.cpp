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

    const auto entry_count_before_bulk_append = state.entries().size();
    for (qsizetype index = 0; index < OperationalLogState::kPageSize + 10; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("test"), QStringLiteral("테스트"),
                          QStringLiteral("ENTRY"), QString::number(index));
    }
    assert(state.entries().size() == entry_count_before_bulk_append + OperationalLogState::kPageSize + 10);

    OperationalLogState overflowing_entries;
    QString oldest_entry_id;
    for (qsizetype index = 0; index < OperationalLogState::kPageSize + 10; ++index) {
        const auto severity = static_cast<OperationalLogSeverity>(index % 4);
        overflowing_entries.appendLocal(severity, QStringLiteral("PI-LOAD-01"), QStringLiteral("부하 로그"),
                                        QStringLiteral("OVERFLOW_ENTRY"), QString::number(index));
        if (index == 0) {
            oldest_entry_id = overflowing_entries.entries().front().id;
        }
    }
    assert(overflowing_entries.entries().size() == OperationalLogState::kPageSize + 10);
    assert(overflowing_entries.unacknowledgedCount() == OperationalLogState::kPageSize + 10);
    assert(overflowing_entries.activeAlertCount() == 254);
    assert(overflowing_entries.entries().back().id == oldest_entry_id);
    assert(overflowing_entries.acknowledge(oldest_entry_id));
    assert(overflowing_entries.unacknowledgedCount() == OperationalLogState::kPageSize + 9);
    assert(overflowing_entries.activeAlertCount() == 254);
    assert(overflowing_entries.acknowledgeAllAlerts() == 254);
    assert(overflowing_entries.unacknowledgedCount() == 255);
    return 0;
}
