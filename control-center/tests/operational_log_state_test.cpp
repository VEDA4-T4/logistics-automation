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

    for (qsizetype index = 0; index < OperationalLogState::kMaximumEntries + 10; ++index) {
        state.appendLocal(OperationalLogSeverity::Info, QStringLiteral("test"), QStringLiteral("테스트"),
                          QStringLiteral("ENTRY"), QString::number(index));
    }
    assert(state.entries().size() == OperationalLogState::kMaximumEntries);
    return 0;
}
