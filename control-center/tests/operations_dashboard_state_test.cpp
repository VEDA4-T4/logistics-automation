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

const logistics::control_center::SensorUnitStatus& SensorById(
    const logistics::control_center::ProcessUnitStatus& process, int sensor_id) {
    for (const auto& sensor : process.sensors) {
        if (sensor.sensor_id == sensor_id)
            return sensor;
    }
    std::abort();
}

}  // namespace

int main() {
    using logistics::control_center::OperationsDashboardState;
    using logistics::control_center::OverallProcessState;

    OperationsDashboardState state;
    assert(state.processes().size() == 5);
    assert(ProcessByKey(state, QStringLiteral("gripper")).display_name == QStringLiteral("그리퍼 이송"));
    assert(ProcessByKey(state, QStringLiteral("gripper")).device_id == QStringLiteral("PI-GRIPPER-01"));
    assert(ProcessByKey(state, QStringLiteral("input")).sensors.size() == 1);
    assert(ProcessByKey(state, QStringLiteral("sorting")).sensors.size() == 3);

    OperationsDashboardState ordered_device_state;
    auto result = ordered_device_state.applyEnvelope(Envelope("SORTING-NORMAL-NEWER", "DEVICE_STATUS",
                                                              DeviceStatus("ONLINE", "SORTING"), "PI-SORTING-01",
                                                              "2026-07-23T01:00:00.300Z"));
    assert(result.applied);
    result =
        ordered_device_state.applyEnvelope(Envelope("SORTING-ERROR-OLDER", "ERROR_OCCURRED",
                                                    { { QStringLiteral("errorCode"), QStringLiteral("ERR-OLD") },
                                                      { QStringLiteral("currentState"), QStringLiteral("ERROR") } },
                                                    "PI-SORTING-01", "2026-07-23T01:00:00.200Z"));
    assert(result.handled && !result.applied);
    assert(!ProcessByKey(ordered_device_state, QStringLiteral("sorting")).has_error);
    assert(ProcessByKey(ordered_device_state, QStringLiteral("sorting")).current_state == QStringLiteral("SORTING"));

    result =
        ordered_device_state.applyEnvelope(Envelope("SORTING-ERROR-NEWER", "ERROR_OCCURRED",
                                                    { { QStringLiteral("errorCode"), QStringLiteral("ERR-NEW") },
                                                      { QStringLiteral("currentState"), QStringLiteral("ERROR") } },
                                                    "PI-SORTING-01", "2026-07-23T01:00:00.400Z"));
    assert(result.applied);
    result = ordered_device_state.applyEnvelope(Envelope("SORTING-NORMAL-OLDER", "DEVICE_STATUS",
                                                         DeviceStatus("ONLINE", "SORTING"), "PI-SORTING-01",
                                                         "2026-07-23T01:00:00.350Z"));
    assert(result.handled && !result.applied);
    assert(ProcessByKey(ordered_device_state, QStringLiteral("sorting")).has_error);
    assert(ProcessByKey(ordered_device_state, QStringLiteral("sorting")).error_code == QStringLiteral("ERR-NEW"));

    result = ordered_device_state.applyEnvelope(Envelope("SORTING-NORMAL-NEWEST", "DEVICE_STATUS",
                                                         DeviceStatus("ONLINE", "SORTING"), "PI-SORTING-01",
                                                         "2026-07-23T01:00:00.500Z"));
    assert(result.applied);
    assert(!ProcessByKey(ordered_device_state, QStringLiteral("sorting")).has_error);
    assert(ProcessByKey(ordered_device_state, QStringLiteral("sorting")).error_code.isEmpty());

    OperationsDashboardState stopped_sensor_state;
    result = stopped_sensor_state.applyEnvelope(
        Envelope("SORTING-STOPPED", "DEVICE_STATUS", DeviceStatus("ONLINE", "STOPPED"), "PI-SORTING-01"));
    assert(result.applied);
    result = stopped_sensor_state.applyEnvelope(
        Envelope("SORTING-SENSOR-2-DETECTED", "SENSOR_STATUS",
                 { { QStringLiteral("sensorId"), 2 },
                   { QStringLiteral("measurementStatus"), QStringLiteral("DETECTED") },
                   { QStringLiteral("distanceCm"), 12 } },
                 "PI-SORTING-01", "2026-07-23T01:00:00.100Z"));
    assert(result.applied);
    const auto sorting_after_detection = ProcessByKey(stopped_sensor_state, QStringLiteral("sorting"));
    assert(sorting_after_detection.current_state == QStringLiteral("STOPPED"));
    assert(SensorById(sorting_after_detection, 2).measurement_status == QStringLiteral("DETECTED"));
    assert(SensorById(sorting_after_detection, 2).distance_cm == 12);
    assert(stopped_sensor_state.overall().state == OverallProcessState::Stopped);

    result = stopped_sensor_state.applyEnvelope(Envelope("SORTING-LEGACY-SENSOR-DETECTED", "DEVICE_STATUS",
                                                         DeviceStatus("ONLINE", "SENSOR_2_DETECTED"), "PI-SORTING-01",
                                                         "2026-07-23T01:00:00.200Z"));
    assert(result.applied);
    assert(ProcessByKey(stopped_sensor_state, QStringLiteral("sorting")).current_state == QStringLiteral("STOPPED"));

    result = stopped_sensor_state.applyEnvelope(Envelope("SORTING-LEGACY-SENSOR-FAULT", "DEVICE_STATUS",
                                                         DeviceStatus("UART_ERROR", "SENSOR_2_FAULT", {}, "ERR-SENSOR"),
                                                         "PI-SORTING-01", "2026-07-23T01:00:00.300Z"));
    assert(result.applied);
    const auto sorting_after_fault = ProcessByKey(stopped_sensor_state, QStringLiteral("sorting"));
    assert(sorting_after_fault.current_state == QStringLiteral("STOPPED"));
    assert(sorting_after_fault.connection_state == logistics::contracts::mqtt::ConnectionState::kOnline);
    assert(sorting_after_fault.has_error);
    assert(SensorById(sorting_after_fault, 2).measurement_status == QStringLiteral("FAULT"));

    result = stopped_sensor_state.applyEnvelope(
        Envelope("SORTING-SENSOR-ERROR", "ERROR_OCCURRED",
                 { { QStringLiteral("errorCode"), QStringLiteral("ERR-SENSOR") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("SENSOR_FAULT") },
                   { QStringLiteral("message"), QStringLiteral("sorting sensor fault") } },
                 "PI-SORTING-01", "2026-07-23T01:00:00.400Z"));
    assert(result.applied);
    assert(ProcessByKey(stopped_sensor_state, QStringLiteral("sorting")).current_state == QStringLiteral("STOPPED"));

    result =
        stopped_sensor_state.applyEnvelope(Envelope("SORTING-SENSOR-2-CLEAR", "SENSOR_STATUS",
                                                    { { QStringLiteral("sensorId"), 2 },
                                                      { QStringLiteral("measurementStatus"), QStringLiteral("CLEAR") },
                                                      { QStringLiteral("distanceCm"), 45 } },
                                                    "PI-SORTING-01", "2026-07-23T01:00:00.500Z"));
    assert(result.applied);
    const auto sorting_after_clear = ProcessByKey(stopped_sensor_state, QStringLiteral("sorting"));
    assert(sorting_after_clear.current_state == QStringLiteral("STOPPED"));
    assert(!sorting_after_clear.has_error);
    assert(sorting_after_clear.error_code.isEmpty());
    assert(SensorById(sorting_after_clear, 2).measurement_status == QStringLiteral("CLEAR"));
    assert(stopped_sensor_state.overall().state == OverallProcessState::Stopped);

    result = stopped_sensor_state.applyEnvelope(
        Envelope("SORTING-SENSOR-1-DETECTED", "SENSOR_STATUS",
                 { { QStringLiteral("sensorId"), 1 },
                   { QStringLiteral("measurementStatus"), QStringLiteral("DETECTED") },
                   { QStringLiteral("distanceCm"), 10 } },
                 "PI-SORTING-01", "2026-07-23T01:00:00.600Z"));
    assert(result.applied);
    result = stopped_sensor_state.applyEnvelope(
        Envelope("SORTING-SENSORS-STALE", "ERROR_OCCURRED",
                 { { QStringLiteral("errorCode"), QStringLiteral("ERR-HEALTH-SENSOR-STALE") },
                   { QStringLiteral("errorLevel"), QStringLiteral("WARNING") },
                   { QStringLiteral("currentState"), QStringLiteral("CONTROLLER_HEALTH") },
                   { QStringLiteral("message"), QStringLiteral("sorting sensors stale") } },
                 "PI-SORTING-01", "2026-07-23T01:00:00.700Z"));
    assert(result.applied);
    const auto sorting_after_stale = ProcessByKey(stopped_sensor_state, QStringLiteral("sorting"));
    assert(sorting_after_stale.current_state == QStringLiteral("STOPPED"));
    assert(sorting_after_stale.has_warning);
    for (const auto& sensor : sorting_after_stale.sensors) {
        assert(sensor.measurement_status == QStringLiteral("UNKNOWN"));
        assert(sensor.distance_cm == -1);
        assert(!sensor.updated_at.isValid());
    }
    assert(stopped_sensor_state.overall().state == OverallProcessState::Stopped);

    result = state.applyEnvelope(
        Envelope("INPUT-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "RUNNING", "WORK-105"), "PI-INPUT-01"));
    assert(result.handled && result.applied && result.error.isEmpty());
    result = state.applyEnvelope(Envelope("VISION-1", "DEVICE_STATUS",
                                          DeviceStatus("ONLINE", "VISION_PROCESSING", "WORK-104"), "PI-VISION-01",
                                          "2026-07-23T01:00:01.000Z"));
    assert(result.applied);
    result =
        state.applyEnvelope(Envelope("GRIPPER-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "TRANSFERRING", "WORK-103"),
                                     "PI-GRIPPER-01", "2026-07-23T01:00:02.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("SORTING-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "SORTING", "WORK-102"),
                                          "PI-SORTING-01", "2026-07-23T01:00:03.000Z"));
    assert(result.applied);
    result = state.applyEnvelope(Envelope("LINE-1", "DEVICE_STATUS", DeviceStatus("ONLINE", "DELIVERING", "WORK-101"),
                                          "PI-LT-01", "2026-07-23T01:00:04.000Z"));
    assert(result.applied);

    assert(ProcessByKey(state, QStringLiteral("input")).work_id == QStringLiteral("WORK-105"));
    assert(ProcessByKey(state, QStringLiteral("vision")).work_id == QStringLiteral("WORK-104"));
    assert(ProcessByKey(state, QStringLiteral("gripper")).work_id == QStringLiteral("WORK-103"));
    assert(ProcessByKey(state, QStringLiteral("sorting")).work_id == QStringLiteral("WORK-102"));
    assert(ProcessByKey(state, QStringLiteral("linetracer")).work_id == QStringLiteral("WORK-101"));
    assert(state.overall().state == OverallProcessState::Running);
    assert(state.overall().active_unit_count == 5);
    assert(state.overall().active_work_count == 5);

    OperationsDashboardState destination_state;
    result = destination_state.applyEnvelope(Envelope(
        "SORTING-ROUTE-ONLINE", "DEVICE_STATUS", DeviceStatus("ONLINE", "SORTING", "WORK-ROUTE"), "PI-SORTING-01"));
    assert(result.applied);
    result = destination_state.applyEnvelope(
        Envelope("DESTINATION-2", "DESTINATION_SET",
                 WorkData("WORK-ROUTE", { { QStringLiteral("destination"), QStringLiteral("2") } }), "central-server",
                 "2026-07-23T01:00:01.000Z"));
    assert(result.applied);
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).work_id == QStringLiteral("WORK-ROUTE"));
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).destination == QStringLiteral("2"));

    result = destination_state.applyEnvelope(
        Envelope("DESTINATION-INVALID", "DESTINATION_SET",
                 WorkData("WORK-ROUTE", { { QStringLiteral("destination"), QStringLiteral("4") } }), "central-server",
                 "2026-07-23T01:00:02.000Z"));
    assert(result.handled && !result.applied && !result.error.isEmpty());
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).destination == QStringLiteral("2"));

    result = destination_state.applyEnvelope(Envelope("SORTING-NEW-WORK", "DEVICE_STATUS",
                                                      DeviceStatus("ONLINE", "SORTING", "WORK-NEXT"), "PI-SORTING-01",
                                                      "2026-07-23T01:00:03.000Z"));
    assert(result.applied);
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).destination.isEmpty());
    result = destination_state.applyEnvelope(
        Envelope("DESTINATION-LATE", "DESTINATION_SET",
                 WorkData("WORK-ROUTE", { { QStringLiteral("destination"), QStringLiteral("route-1") } }),
                 "central-server", "2026-07-23T01:00:04.000Z"));
    assert(result.handled && !result.applied);
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).work_id == QStringLiteral("WORK-NEXT"));
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).destination.isEmpty());

    result = destination_state.applyEnvelope(
        Envelope("DESTINATION-3", "DESTINATION_SET",
                 WorkData("WORK-NEXT", { { QStringLiteral("destination"), QStringLiteral("destination-3") } }),
                 "central-server", "2026-07-23T01:00:05.000Z"));
    assert(result.applied);
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).destination == QStringLiteral("destination-3"));
    result = destination_state.applyEnvelope(Envelope("SORTING-ROUTE-OFFLINE", "DEVICE_STATUS",
                                                      DeviceStatus("OFFLINE", "DISCONNECTED", "WORK-NEXT"),
                                                      "PI-SORTING-01", "2026-07-23T01:00:05.500Z"));
    assert(result.applied);
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).destination.isEmpty());
    destination_state.markMqttDisconnected(
        QDateTime::fromString(QStringLiteral("2026-07-23T01:00:06.000Z"), Qt::ISODateWithMs));
    assert(ProcessByKey(destination_state, QStringLiteral("sorting")).destination.isEmpty());
    result = destination_state.applyEnvelope(Envelope("LINE-AFTER-ROUTE-RESET", "DEVICE_STATUS",
                                                      DeviceStatus("ONLINE", "DELIVERING", "WORK-NEXT"), "PI-LT-01",
                                                      "2026-07-23T01:00:07.000Z"));
    assert(result.applied);
    assert(ProcessByKey(destination_state, QStringLiteral("linetracer")).destination.isEmpty());

    OperationsDashboardState concurrent_route_state;
    result = concurrent_route_state.applyEnvelope(
        Envelope("SORTING-A", "DEVICE_STATUS", DeviceStatus("ONLINE", "SORTING", "WORK-A"), "PI-SORTING-01"));
    assert(result.applied);
    result = concurrent_route_state.applyEnvelope(
        Envelope("DESTINATION-A", "DESTINATION_SET",
                 WorkData("WORK-A", { { QStringLiteral("destination"), QStringLiteral("route-1") } }), "central-server",
                 "2026-07-23T01:00:01.000Z"));
    assert(result.applied);
    result = concurrent_route_state.applyEnvelope(Envelope("SORTING-B", "DEVICE_STATUS",
                                                           DeviceStatus("ONLINE", "SORTING", "WORK-B"), "PI-SORTING-01",
                                                           "2026-07-23T01:00:02.000Z"));
    assert(result.applied);
    assert(ProcessByKey(concurrent_route_state, QStringLiteral("sorting")).work_id == QStringLiteral("WORK-B"));
    assert(ProcessByKey(concurrent_route_state, QStringLiteral("sorting")).destination.isEmpty());

    result = concurrent_route_state.applyEnvelope(Envelope("LINE-A", "DEVICE_STATUS",
                                                           DeviceStatus("ONLINE", "DELIVERING", "WORK-A"), "PI-LT-01",
                                                           "2026-07-23T01:00:03.000Z"));
    assert(result.applied);
    const auto line_a = ProcessByKey(concurrent_route_state, QStringLiteral("linetracer"));
    assert(line_a.work_id == QStringLiteral("WORK-A"));
    assert(line_a.destination == QStringLiteral("route-1"));
    assert(!line_a.work_completed);

    result = concurrent_route_state.applyEnvelope(
        Envelope("WORK-A-COMPLETE", "WORK_COMPLETED",
                 WorkData("WORK-A", { { QStringLiteral("result"), QStringLiteral("SUCCESS") } }), "central-server",
                 "2026-07-23T01:00:04.000Z"));
    assert(result.applied);
    const auto completed_line_a = ProcessByKey(concurrent_route_state, QStringLiteral("linetracer"));
    assert(completed_line_a.current_state == QStringLiteral("배송 완료"));
    assert(completed_line_a.destination == QStringLiteral("route-1"));
    assert(completed_line_a.work_completed);

    OperationsDashboardState failed_completion_state;
    result = failed_completion_state.applyEnvelope(Envelope(
        "DESTINATION-FAILED-WORK", "DESTINATION_SET",
        WorkData("WORK-FAILED", { { QStringLiteral("destination"), QStringLiteral("2") } }), "central-server"));
    assert(result.applied);
    result = failed_completion_state.applyEnvelope(Envelope("LINE-FAILED-WORK", "DEVICE_STATUS",
                                                            DeviceStatus("ONLINE", "DELIVERING", "WORK-FAILED"),
                                                            "PI-LT-01", "2026-07-23T01:00:01.000Z"));
    assert(result.applied);
    result = failed_completion_state.applyEnvelope(
        Envelope("WORK-FAILED-COMPLETE", "WORK_COMPLETED",
                 WorkData("WORK-FAILED", { { QStringLiteral("result"), QStringLiteral("FAILED") } }), "central-server",
                 "2026-07-23T01:00:02.000Z"));
    assert(result.applied);
    const auto failed_line = ProcessByKey(failed_completion_state, QStringLiteral("linetracer"));
    assert(failed_line.destination == QStringLiteral("2"));
    assert(failed_line.work_completed);
    assert(failed_line.has_error);
    failed_completion_state.markMqttDisconnected(
        QDateTime::fromString(QStringLiteral("2026-07-23T01:00:03.000Z"), Qt::ISODateWithMs));
    const auto reset_failed_line = ProcessByKey(failed_completion_state, QStringLiteral("linetracer"));
    assert(reset_failed_line.destination.isEmpty());
    assert(!reset_failed_line.work_completed);

    result = concurrent_route_state.applyEnvelope(Envelope("LINE-C", "DEVICE_STATUS",
                                                           DeviceStatus("ONLINE", "DELIVERING", "WORK-C"), "PI-LT-01",
                                                           "2026-07-23T01:00:05.000Z"));
    assert(result.applied);
    const auto line_c = ProcessByKey(concurrent_route_state, QStringLiteral("linetracer"));
    assert(line_c.work_id == QStringLiteral("WORK-C"));
    assert(line_c.destination.isEmpty());
    assert(!line_c.work_completed);

    OperationsDashboardState bounded_route_state;
    const auto route_cache_start = QDateTime::fromString(QStringLiteral("2026-07-23T03:00:00.000Z"), Qt::ISODateWithMs);
    for (int index = 0; index <= 512; ++index) {
        const auto suffix = QString::number(index);
        result = bounded_route_state.applyEnvelope(
            Envelope(QStringLiteral("DESTINATION-CACHE-%1").arg(suffix), "DESTINATION_SET",
                     WorkData(QStringLiteral("WORK-CACHE-%1").arg(suffix),
                              { { QStringLiteral("destination"), QString::number((index % 3) + 1) } }),
                     "central-server", route_cache_start.addMSecs(index).toString(Qt::ISODateWithMs)));
        assert(result.applied);
    }
    result = bounded_route_state.applyEnvelope(
        Envelope("LINE-EVICTED-ROUTE", "DEVICE_STATUS", DeviceStatus("ONLINE", "DELIVERING", "WORK-CACHE-0"),
                 "PI-LT-01", route_cache_start.addSecs(1).toString(Qt::ISODateWithMs)));
    assert(result.applied);
    assert(ProcessByKey(bounded_route_state, QStringLiteral("linetracer")).destination.isEmpty());
    result = bounded_route_state.applyEnvelope(
        Envelope("LINE-RETAINED-ROUTE", "DEVICE_STATUS", DeviceStatus("ONLINE", "DELIVERING", "WORK-CACHE-512"),
                 "PI-LT-01", route_cache_start.addSecs(2).toString(Qt::ISODateWithMs)));
    assert(result.applied);
    assert(ProcessByKey(bounded_route_state, QStringLiteral("linetracer")).destination == QStringLiteral("3"));

    OperationsDashboardState stale_destination_state;
    result = stale_destination_state.applyEnvelope(Envelope("SORTING-STALE-ROUTE", "DEVICE_STATUS",
                                                            DeviceStatus("ONLINE", "SORTING", "WORK-STALE-ROUTE"),
                                                            "PI-SORTING-01"));
    assert(result.applied);
    result = stale_destination_state.applyEnvelope(
        Envelope("DESTINATION-STALE-ROUTE", "DESTINATION_SET",
                 WorkData("WORK-STALE-ROUTE", { { QStringLiteral("destination"), QStringLiteral("start-1") } }),
                 "central-server", "2026-07-23T01:00:01.000Z"));
    assert(result.applied);
    assert(stale_destination_state.expireStaleProcesses(
        QDateTime::fromString(QStringLiteral("2026-07-23T01:00:15.000Z"), Qt::ISODateWithMs)));
    assert(ProcessByKey(stale_destination_state, QStringLiteral("sorting")).destination.isEmpty());

    result = state.applyEnvelope(
        Envelope("SORTING-SENSOR-STALE", "ERROR_OCCURRED",
                 { { QStringLiteral("errorCode"), QStringLiteral("err_health_sensor_stale") },
                   { QStringLiteral("errorLevel"), QStringLiteral("ERROR") },
                   { QStringLiteral("currentState"), QStringLiteral("CONTROLLER_HEALTH") },
                   { QStringLiteral("message"), QStringLiteral("sorting controller reported a stale sensor") } },
                 "PI-SORTING-01", "2026-07-23T01:00:04.250Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("sorting")).has_warning);
    assert(!ProcessByKey(state, QStringLiteral("sorting")).has_error);
    assert(ProcessByKey(state, QStringLiteral("sorting")).current_state == QStringLiteral("SORTING"));
    assert(ProcessByKey(state, QStringLiteral("sorting")).error_code == QStringLiteral("err_health_sensor_stale"));
    assert(state.overall().state == OverallProcessState::Running);

    result = state.applyEnvelope(
        Envelope("SORTING-SENSOR-STALE-STATUS", "DEVICE_STATUS",
                 DeviceStatus("UART_ERROR", "CONTROLLER_HEALTH", "WORK-102", "ERR-HEALTH-SENSOR-STALE"),
                 "PI-SORTING-01", "2026-07-23T01:00:04.375Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("sorting")).has_warning);
    assert(!ProcessByKey(state, QStringLiteral("sorting")).has_error);
    assert(ProcessByKey(state, QStringLiteral("sorting")).connection_state ==
           logistics::contracts::mqtt::ConnectionState::kOnline);
    assert(ProcessByKey(state, QStringLiteral("sorting")).current_state == QStringLiteral("SORTING"));
    assert(state.overall().state == OverallProcessState::Running);

    result = state.applyEnvelope(Envelope("SORTING-SENSOR-RECOVERED", "DEVICE_STATUS",
                                          DeviceStatus("ONLINE", "SORTING", "WORK-102"), "PI-SORTING-01",
                                          "2026-07-23T01:00:04.500Z"));
    assert(result.applied);
    assert(!ProcessByKey(state, QStringLiteral("sorting")).has_warning);
    assert(ProcessByKey(state, QStringLiteral("sorting")).error_code.isEmpty());

    OperationsDashboardState mqtt_transition_state;
    result = mqtt_transition_state.applyEnvelope(
        Envelope("MQTT-INPUT", "DEVICE_STATUS", DeviceStatus("ONLINE", "RUNNING", "WORK-MQTT"), "PI-INPUT-01"));
    assert(result.applied);
    const auto disconnected_at = QDateTime::fromString(QStringLiteral("2026-07-23T01:00:04.500Z"), Qt::ISODateWithMs);
    mqtt_transition_state.markMqttDisconnected(disconnected_at);
    for (const auto& process : mqtt_transition_state.processes()) {
        assert(process.connection_state == logistics::contracts::mqtt::ConnectionState::kUnknown);
        assert(process.current_state == QStringLiteral("DISCONNECTED"));
        assert(process.work_id.isEmpty());
        assert(process.destination.isEmpty());
        assert(!process.work_completed);
        assert(process.error_code.isEmpty());
        assert(!process.has_error);
        assert(!process.has_warning);
        assert(process.updated_at == disconnected_at);
        for (const auto& sensor : process.sensors) {
            assert(sensor.measurement_status == QStringLiteral("UNKNOWN"));
            assert(sensor.distance_cm == -1);
            assert(!sensor.updated_at.isValid());
        }
    }
    assert(mqtt_transition_state.overall().state == OverallProcessState::Idle);
    assert(mqtt_transition_state.overall().stage == QStringLiteral("공정 상태 수신 대기"));
    assert(mqtt_transition_state.overall().detail == QStringLiteral("MQTT 연결 끊김"));
    assert(mqtt_transition_state.overall().active_unit_count == 0);
    assert(mqtt_transition_state.overall().active_work_count == 0);

    const auto reconnected_at = disconnected_at.addSecs(1);
    mqtt_transition_state.markMqttConnectedAwaitingStatus(reconnected_at);
    for (const auto& process : mqtt_transition_state.processes()) {
        assert(process.connection_state == logistics::contracts::mqtt::ConnectionState::kUnknown);
        assert(process.current_state == QStringLiteral("상태 수신 대기"));
        assert(!process.has_error);
        assert(!process.has_warning);
    }
    assert(mqtt_transition_state.overall().stage == QStringLiteral("공정 상태 수신 대기"));
    assert(mqtt_transition_state.overall().detail == QStringLiteral("MQTT 연결됨 · 노드 상태 수신 대기"));

    result = mqtt_transition_state.applyEnvelope(Envelope("INPUT-RESTORED", "DEVICE_STATUS",
                                                          DeviceStatus("ONLINE", "RUNNING", "WORK-MQTT"), "PI-INPUT-01",
                                                          "2026-07-23T01:00:06.000Z"));
    assert(result.applied);
    assert(ProcessByKey(mqtt_transition_state, QStringLiteral("input")).connection_state ==
           logistics::contracts::mqtt::ConnectionState::kOnline);
    assert(ProcessByKey(mqtt_transition_state, QStringLiteral("input")).current_state == QStringLiteral("RUNNING"));

    OperationsDashboardState heartbeat_expiration_state;
    result = heartbeat_expiration_state.applyEnvelope(Envelope("VISION-ONLINE", "DEVICE_STATUS",
                                                               DeviceStatus("ONLINE", "WAITING_FOR_PRODUCT"),
                                                               "PI-VISION-01", "2026-07-23T01:00:00.000Z"));
    assert(result.applied);
    assert(!heartbeat_expiration_state.expireStaleProcesses(
        QDateTime::fromString(QStringLiteral("2026-07-23T01:00:14.999Z"), Qt::ISODateWithMs)));
    assert(heartbeat_expiration_state.expireStaleProcesses(
        QDateTime::fromString(QStringLiteral("2026-07-23T01:00:15.000Z"), Qt::ISODateWithMs)));
    const auto expired_vision = ProcessByKey(heartbeat_expiration_state, QStringLiteral("vision"));
    assert(expired_vision.connection_state == logistics::contracts::mqtt::ConnectionState::kOffline);
    assert(expired_vision.current_state == QStringLiteral("DISCONNECTED"));
    assert(expired_vision.work_id.isEmpty());
    assert(expired_vision.error_code == QStringLiteral("ERR-HEARTBEAT-TIMEOUT"));

    OperationsDashboardState clock_skew_state;
    const auto received_at = QDateTime::fromString(QStringLiteral("2026-07-23T02:00:00.000Z"), Qt::ISODateWithMs);
    result = clock_skew_state.applyEnvelope(
        Envelope("VISION-SKEWED", "DEVICE_STATUS", DeviceStatus("ONLINE", "WAITING_FOR_PRODUCT"), "PI-VISION-01",
                 "2026-07-23T01:55:00.000Z"),
        received_at);
    assert(result.applied);
    assert(!clock_skew_state.expireStaleProcesses(received_at.addMSecs(14999)));
    assert(clock_skew_state.expireStaleProcesses(received_at.addSecs(15)));
    result = clock_skew_state.applyEnvelope(Envelope("VISION-SKEWED-STALE-ERROR", "ERROR_OCCURRED",
                                                     { { QStringLiteral("errorCode"), QStringLiteral("ERR-OLD") },
                                                       { QStringLiteral("currentState"), QStringLiteral("ERROR") } },
                                                     "PI-VISION-01", "2026-07-23T01:54:59.000Z"),
                                            received_at.addSecs(16));
    assert(result.handled && !result.applied);
    assert(ProcessByKey(clock_skew_state, QStringLiteral("vision")).error_code ==
           QStringLiteral("ERR-HEARTBEAT-TIMEOUT"));
    result = clock_skew_state.applyEnvelope(
        Envelope("VISION-SKEWED-STALE-STATUS", "DEVICE_STATUS", DeviceStatus("ONLINE", "WAITING_FOR_PRODUCT"),
                 "PI-VISION-01", "2026-07-23T01:54:58.000Z"),
        received_at.addSecs(17));
    assert(result.handled && !result.applied);
    assert(ProcessByKey(clock_skew_state, QStringLiteral("vision")).connection_state ==
           logistics::contracts::mqtt::ConnectionState::kOffline);
    result = clock_skew_state.applyEnvelope(
        Envelope("VISION-SKEWED-STALE-HEARTBEAT", "HEARTBEAT", DeviceStatus("ONLINE", "WAITING_FOR_PRODUCT"),
                 "PI-VISION-01", "2026-07-23T01:54:57.000Z"),
        received_at.addSecs(18));
    assert(result.handled && !result.applied);
    assert(ProcessByKey(clock_skew_state, QStringLiteral("vision")).connection_state ==
           logistics::contracts::mqtt::ConnectionState::kOffline);
    result = clock_skew_state.applyEnvelope(
        Envelope("VISION-SKEWED-RECOVERED", "DEVICE_STATUS", DeviceStatus("ONLINE", "WAITING_FOR_PRODUCT"),
                 "PI-VISION-01", "2026-07-23T01:55:05.000Z"),
        received_at.addSecs(19));
    assert(result.applied);
    assert(ProcessByKey(clock_skew_state, QStringLiteral("vision")).connection_state ==
           logistics::contracts::mqtt::ConnectionState::kOnline);

    OperationsDashboardState operational_waiting_state;
    result = operational_waiting_state.applyEnvelope(
        Envelope("VISION-WAITING", "DEVICE_STATUS", DeviceStatus("ONLINE", "WAITING_FOR_PRODUCT"), "PI-VISION-01"));
    assert(result.applied);
    assert(operational_waiting_state.overall().state == OverallProcessState::Running);
    assert(operational_waiting_state.overall().active_unit_count == 0);
    assert(operational_waiting_state.overall().active_work_count == 0);
    assert(operational_waiting_state.overall().stage == QStringLiteral("가동 준비 완료 · 상품 대기 1"));

    result = state.applyEnvelope(Envelope("LEGACY-ROBOT", "DEVICE_STATUS", DeviceStatus("ONLINE", "PICKING"),
                                          "PI-ROBOT-01", "2026-07-23T01:00:02.500Z"));
    assert(result.handled && !result.applied);

    result = state.applyEnvelope(
        Envelope("INPUT-2", "WORK_CREATED", WorkData("WORK-106"), "central-server", "2026-07-23T01:00:05.000Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("input")).work_id == QStringLiteral("WORK-106"));
    assert(ProcessByKey(state, QStringLiteral("vision")).work_id == QStringLiteral("WORK-104"));
    assert(ProcessByKey(state, QStringLiteral("gripper")).work_id == QStringLiteral("WORK-103"));

    result = state.applyEnvelope(
        Envelope("VISION-EVENT", "PRODUCT_INFO", WorkData("WORK-104"), "central-server", "2026-07-23T01:00:05.500Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("vision")).current_state == QStringLiteral("상품 이동 준비"));
    assert(ProcessByKey(state, QStringLiteral("gripper")).current_state == QStringLiteral("TRANSFERRING"));

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
    assert(ProcessByKey(state, QStringLiteral("gripper")).work_id == QStringLiteral("WORK-103"));
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
    result = state.applyEnvelope(Envelope("GRIPPER-IDLE", "DEVICE_STATUS", DeviceStatus("ONLINE", "IDLE"),
                                          "PI-GRIPPER-01", "2026-07-23T01:00:12.000Z"));
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

    result = state.applyEnvelope(Envelope("COMMAND-2-PROCESSING", "COMMAND_RESPONSE",
                                          { { QStringLiteral("requestId"), QStringLiteral("REQ-2") },
                                            { QStringLiteral("command"), QStringLiteral("RECOVERY") },
                                            { QStringLiteral("result"), QStringLiteral("PROCESSING") } },
                                          "central-server", "2026-07-23T01:00:14.000Z"));
    assert(result.applied && state.overall().state == OverallProcessState::Recovery);

    result = state.applyEnvelope(Envelope("COMMAND-2-SUCCESS", "COMMAND_RESPONSE",
                                          { { QStringLiteral("requestId"), QStringLiteral("REQ-2") },
                                            { QStringLiteral("command"), QStringLiteral("RECOVERY") },
                                            { QStringLiteral("result"), QStringLiteral("SUCCESS") } },
                                          "central-server", "2026-07-23T01:00:14.250Z"));
    assert(result.applied && state.overall().state == OverallProcessState::Stopped);
    assert(state.overall().stage == QStringLiteral("복구 완료 · 시작 대기"));

    OperationsDashboardState individual_command_state;
    result = individual_command_state.applyEnvelope(
        Envelope("VISION-RUNNING", "DEVICE_STATUS", DeviceStatus("ONLINE", "WAITING_FOR_PRODUCT"), "PI-VISION-01"));
    assert(result.applied && individual_command_state.overall().state == OverallProcessState::Running);
    result = individual_command_state.applyEnvelope(
        Envelope("INDIVIDUAL-STOP", "COMMAND_RESPONSE",
                 { { QStringLiteral("requestId"), QStringLiteral("REQ-INDIVIDUAL") },
                   { QStringLiteral("command"), QStringLiteral("STOP") },
                   { QStringLiteral("result"), QStringLiteral("SUCCESS") } },
                 "central-server", "2026-07-23T01:00:01.000Z"),
        {}, false);
    assert(result.handled && !result.applied);
    assert(individual_command_state.overall().state == OverallProcessState::Running);

    result = state.applyEnvelope(Envelope("VISION-RECOVERED", "DEVICE_STATUS", DeviceStatus("ONLINE", "STOPPED"),
                                          "PI-VISION-01", "2026-07-23T01:00:14.500Z"));
    assert(result.applied);
    assert(ProcessByKey(state, QStringLiteral("vision")).current_state == QStringLiteral("STOPPED"));
    assert(state.overall().state == OverallProcessState::Stopped);

    result = state.applyEnvelope(Envelope("COMMAND-2-SUCCESS", "COMMAND_RESPONSE",
                                          { { QStringLiteral("requestId"), QStringLiteral("REQ-2") },
                                            { QStringLiteral("command"), QStringLiteral("STOP") },
                                            { QStringLiteral("result"), QStringLiteral("SUCCESS") } },
                                          "central-server", "2026-07-23T01:00:15.000Z"));
    assert(result.handled && !result.applied);
    assert(state.overall().state == OverallProcessState::Stopped);

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
