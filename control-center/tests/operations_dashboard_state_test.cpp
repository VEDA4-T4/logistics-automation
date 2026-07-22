#include "logistics/control_center/operations_dashboard_state.hpp"

#include <QJsonObject>
#include <QString>
#include <cassert>
#include <cstdlib>

namespace {

QJsonObject Envelope(const QString& message_id, const QString& message_type, QJsonObject data,
                     const QString& source_id = QStringLiteral("PI-INPUT-01"),
                     const QString& timestamp = QStringLiteral("2026-07-23T01:00:00.000Z")) {
    return {
        { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
        { QStringLiteral("messageId"), message_id },
        { QStringLiteral("messageType"), message_type },
        { QStringLiteral("sourceId"), source_id },
        { QStringLiteral("timestamp"), timestamp },
        { QStringLiteral("data"), data },
    };
}

QJsonObject DeviceStatus(const QString& status, const QString& current_state, const QString& work_id = {},
                         const QString& error_code = {}) {
    QJsonObject data{
        { QStringLiteral("status"), status },
        { QStringLiteral("currentState"), current_state },
    };
    if (!work_id.isEmpty())
        data.insert(QStringLiteral("jobId"), work_id);
    if (!error_code.isEmpty())
        data.insert(QStringLiteral("errorCode"), error_code);
    return data;
}

QJsonObject WorkData(const QString& work_id, QJsonObject data = {}) {
    data.insert(QStringLiteral("workId"), work_id);
    return data;
}

const logistics::control_center::ProcessUnitStatus& ProcessByKey(
    const logistics::control_center::OperationsDashboardState& state, const QString& key) {
    for (const auto& process : state.processes()) {
        if (process.key == key)
            return process;
    }
    std::abort();
}

}  // namespace

int main() {
    using logistics::control_center::OperationsDashboardState;
    using logistics::control_center::OverallProcessState;

    OperationsDashboardState state;
    assert(state.processes().size() == 5);

    auto result = state.applyEnvelope(
        Envelope("INPUT-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "RUNNING", "WORK-105"), "PI-INPUT-01"));
    assert(result.handled && result.applied && result.error.isEmpty());
    result = state.applyEnvelope(Envelope("VISION-1", "DEVICE_STATUS",
                                          DeviceStatus("ONLINE", "VISION_PROCESSING", "WORK-104"), "PI-VISION-01",
                                          "2026-07-23T01:00:01.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("ROBOT-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "PICKING", "WORK-103"),
                                          "PI-ROBOT-01", "2026-07-23T01:00:02.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("SORTING-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "SORTING", "WORK-102"),
                                          "PI-SORTING-01", "2026-07-23T01:00:03.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("LINE-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "DELIVERING", "WORK-101"),
                                          "PI-LT-01", "2026-07-23T01:00:04.000Z"));
    assert(result.applied);

    assert(ProcessByKey(state, QStringLiteral("input")).work_id == QStringLiteral("WORK-105"));
    assert(ProcessByKey(state, QStringLiteral("vision")).work_id == QStringLiteral("WORK-104"));
    assert(ProcessByKey(state, QStringLiteral("robot_arm")).work_id == QStringLiteral("WORK-103"));
    assert(ProcessByKey(state, QStringLiteral("sorting")).work_id == QStringLiteral("WORK-102"));
    assert(ProcessByKey(state, QStringLiteral("linetracer")).work_id == QStringLiteral("WORK-101"));
    assert(state.overall().state == OverallProcessState::Running);
    assert(state.overall().active_unit_count == 5);
    assert(state.overall().active_work_count == 5);

    result = state.applyEnvelope(
        Envelope("INPUT-2", "WORK_CREATED", WorkData("WORK-106"), "central-server", "2026-07-23T01:00:05.000Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("input")).work_id == QStringLiteral("WORK-106"));
    assert(ProcessByKey(state, QStringLiteral("vision")).work_id == QStringLiteral("WORK-104"));
    assert(ProcessByKey(state, QStringLiteral("robot_arm")).work_id == QStringLiteral("WORK-103"));

    result = state.applyEnvelope(
        Envelope("VISION-EVENT", "PRODUCT_INFO", WorkData("WORK-104"), "central-server", "2026-07-23T01:00:05.500Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("vision")).current_state == QStringLiteral("상품 이동 준비"));
    assert(ProcessByKey(state, QStringLiteral("robot_arm")).current_state == QStringLiteral("PICKING"));

    result = state.applyEnvelope(
        Envelope("INPUT-LATE", "WORK_CREATED", WorkData("WORK-105"), "central-server", "2026-07-23T01:00:06.000Z"));
    assert(result.handled && !result.applied);
    assert(ProcessByKey(state, QStringLiteral("input")).work_id == QStringLiteral("WORK-106"));

    result = state.applyEnvelope(Envelope("SORTING-ERROR", "ERROR_OCCURRED",
                                          { { QStringLiteral("jobId"), QStringLiteral("WORK-102") },
                                            { QStringLiteral("errorCode"), QStringLiteral("SENSOR_TIMEOUT") },
                                            { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                                            { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                                            { QStringLiteral("message"), QStringLiteral("분류 센서 응답 없음") } },
                                          "PI-SORTING-01", "2026-07-23T01:00:07.000Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("sorting")).has_error);
    assert(ProcessByKey(state, QStringLiteral("robot_arm")).work_id == QStringLiteral("WORK-103"));
    assert(state.overall().state == OverallProcessState::Error);

    result = state.applyEnvelope(Envelope("SORTING-RECOVERED", "DEVICE_STATUS",
                                          DeviceStatus("ONLINE", "SORTING", "WORK-102"), "PI-SORTING-01",
                                          "2026-07-23T01:00:08.000Z"));
    assert(result.applied && !ProcessByKey(state, QStringLiteral("sorting")).has_error);
    assert(state.overall().state == OverallProcessState::Running);

    result =
        state.applyEnvelope(Envelope("WORK-COMPLETE", "WORK_COMPLETED",
                                     WorkData("WORK-101", { { QStringLiteral("result"), QStringLiteral("SUCCESS") } }),
                                     "central-server", "2026-07-23T01:00:09.000Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("linetracer")).current_state == QStringLiteral("배송 완료"));
    assert(state.overall().state == OverallProcessState::Running);
    assert(state.overall().active_unit_count == 4);

    result = state.applyEnvelope(Envelope("INPUT-IDLE", "DEVICE_STATUS", DeviceStatus("ONLINE", "IDLE"), "PI-INPUT-01",
                                          "2026-07-23T01:00:10.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("VISION-IDLE", "DEVICE_STATUS", DeviceStatus("ONLINE", "IDLE"),
                                          "PI-VISION-01", "2026-07-23T01:00:11.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("ROBOT-IDLE", "DEVICE_STATUS", DeviceStatus("ONLINE", "IDLE"), "PI-ROBOT-01",
                                          "2026-07-23T01:00:12.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("SORTING-IDLE", "DEVICE_STATUS", DeviceStatus("ONLINE", "IDLE"),
                                          "PI-SORTING-01", "2026-07-23T01:00:13.000Z"));
    assert(result.applied);
    assert(state.overall().state == OverallProcessState::Completed);
    assert(state.overall().active_unit_count == 0);
    assert(state.overall().active_work_count == 0);

    result = state.applyEnvelope(Envelope("UNKNOWN-DEVICE", "DEVICE_STATUS", DeviceStatus("ONLINE", "RUNNING"),
                                          "PI-UNKNOWN-01", "2026-07-23T01:00:12.000Z"));
    assert(result.handled && !result.applied && result.error.isEmpty());

    result = state.applyEnvelope(Envelope("COMMAND-1", "COMMAND_RESPONSE",
                                          { { QStringLiteral("requestId"), QStringLiteral("REQ-1") },
                                            { QStringLiteral("command"), QStringLiteral("EMERGENCY_STOP") },
                                            { QStringLiteral("result"), QStringLiteral("SUCCESS") } },
                                          "central-server", "2026-07-23T01:00:13.000Z"));
    assert(result.applied && state.overall().state == OverallProcessState::EmergencyStop);

    result = state.applyEnvelope(Envelope("INPUT-AFTER-ESTOP", "DEVICE_STATUS", DeviceStatus("ONLINE", "IDLE"),
                                          "PI-INPUT-01", "2026-07-23T01:00:13.500Z"));
    assert(result.applied && state.overall().state == OverallProcessState::EmergencyStop);

    result = state.applyEnvelope(Envelope("COMMAND-2", "COMMAND_RESPONSE",
                                          { { QStringLiteral("requestId"), QStringLiteral("REQ-2") },
                                            { QStringLiteral("command"), QStringLiteral("RECOVERY") },
                                            { QStringLiteral("result"), QStringLiteral("SUCCESS") } },
                                          "central-server", "2026-07-23T01:00:14.000Z"));
    assert(result.applied && state.overall().state == OverallProcessState::Recovery);

    result = state.applyEnvelope(Envelope("COMMAND-2", "COMMAND_RESPONSE",
                                          { { QStringLiteral("requestId"), QStringLiteral("REQ-2") },
                                            { QStringLiteral("command"), QStringLiteral("STOP") },
                                            { QStringLiteral("result"), QStringLiteral("SUCCESS") } },
                                          "central-server", "2026-07-23T01:00:15.000Z"));
    assert(result.handled && !result.applied);
    assert(state.overall().state == OverallProcessState::Recovery);

    OperationsDashboardState configured_state;
    auto definitions = logistics::control_center::DefaultProcessDefinitions();
    definitions[0].device_id = QStringLiteral("CUSTOM-INPUT");
    configured_state.configureProcesses(definitions);
    result = configured_state.applyEnvelope(
        Envelope("CUSTOM-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "RUNNING", "WORK-CUSTOM"), "CUSTOM-INPUT"));
    assert(result.applied);
    assert(ProcessByKey(configured_state, QStringLiteral("input")).work_id == QStringLiteral("WORK-CUSTOM"));

    return 0;
}
