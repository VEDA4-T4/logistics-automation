#include "logistics/control_center/operations_dashboard_panel.hpp"

#include <QApplication>
#include <QFrame>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollArea>
#include <cassert>

namespace {

QJsonObject DeviceEnvelope(const QString& message_id, const QString& source_id, const QString& state,
                           const QString& work_id, int second) {
    QJsonObject data{
        { QStringLiteral("status"), QStringLiteral("ONLINE") },
        { QStringLiteral("currentState"), state },
    };
    if (!work_id.isEmpty()) {
        data.insert(QStringLiteral("jobId"), work_id);
    }
    return {
        { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
        { QStringLiteral("messageId"), message_id },
        { QStringLiteral("messageType"), QStringLiteral("DEVICE_STATUS") },
        { QStringLiteral("sourceId"), source_id },
        { QStringLiteral("timestamp"),
          QStringLiteral("2026-07-23T01:00:%1.000Z").arg(second, 2, 10, QLatin1Char('0')) },
        { QStringLiteral("data"), data },
    };
}

QJsonObject SensorEnvelope(const QString& message_id, const QString& source_id, int sensor_id,
                           const QString& measurement_status, int distance_cm, int second) {
    return {
        { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
        { QStringLiteral("messageId"), message_id },
        { QStringLiteral("messageType"), QStringLiteral("SENSOR_STATUS") },
        { QStringLiteral("sourceId"), source_id },
        { QStringLiteral("timestamp"),
          QStringLiteral("2026-07-23T01:00:%1.500Z").arg(second, 2, 10, QLatin1Char('0')) },
        { QStringLiteral("data"),
          QJsonObject{
              { QStringLiteral("sensorId"), sensor_id },
              { QStringLiteral("measurementStatus"), measurement_status },
              { QStringLiteral("distanceCm"), distance_cm },
          } },
    };
}

}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::OperationsDashboardState state;
    assert(state.applyEnvelope(DeviceEnvelope("INPUT", "PI-INPUT-01", "RUNNING", "WORK-105", 0)).applied);
    assert(state.applyEnvelope(DeviceEnvelope("VISION", "PI-VISION-01", "VISION_PROCESSING", "WORK-104", 1)).applied);
    assert(state.applyEnvelope(DeviceEnvelope("GRIPPER", "PI-GRIPPER-01", "TRANSFERRING", "WORK-103", 2)).applied);
    assert(state.applyEnvelope(DeviceEnvelope("SORTING", "PI-SORTING-01", "SORTING", "WORK-102", 3)).applied);
    assert(state.applyEnvelope(DeviceEnvelope("LINE", "PI-LT-01", "DELIVERING", "WORK-101", 4)).applied);

    logistics::control_center::OperationsDashboardPanel panel;
    panel.resize(1280, 300);
    panel.setState(state);
    panel.setMqttConnected(true);
    panel.show();
    application.processEvents();

    assert(panel.findChild<QScrollArea*>(QStringLiteral("processStatusSection")) == nullptr);
    const auto cards =
        panel.findChildren<QFrame*>(QRegularExpression(QStringLiteral("(overallProcessCard|processUnitCard)")));
    assert(cards.size() == 6);
    const int top = cards.front()->mapTo(&panel, QPoint{}).y();
    for (const auto* card : cards) {
        assert(qAbs(card->mapTo(&panel, QPoint{}).y() - top) <= 2);
    }
    assert(panel.maximumHeight() <= 140);
    assert(panel.findChild<QWidget*>(QStringLiteral("processCardGrid")) != nullptr);
    assert(panel.findChild<QFrame*>(QStringLiteral("conveyorSystemGroup")) == nullptr);
    assert(panel.findChildren<QLabel*>(QStringLiteral("sensorStatusIndicator")).size() == 4);
    int working_statuses = 0;
    int running_statuses = 0;
    for (const auto* status : panel.findChildren<QLabel*>(QStringLiteral("processVisualStatus"))) {
        if (status->text() == QStringLiteral("작업 중")) {
            assert(status->styleSheet().contains(QStringLiteral("#75beff")));
            ++working_statuses;
        } else if (status->text() == QStringLiteral("가동 중")) {
            assert(status->styleSheet().contains(QStringLiteral("#89d185")));
            ++running_statuses;
        }
    }
    assert(working_statuses == 4);
    assert(running_statuses == 1);
    QLabel* sorting_sensor_2 = nullptr;
    for (auto* indicator : panel.findChildren<QLabel*>(QStringLiteral("sensorStatusIndicator"))) {
        if (indicator->property("sensorId").toInt() == 2) {
            sorting_sensor_2 = indicator;
            break;
        }
    }
    assert(sorting_sensor_2 != nullptr);
    assert(sorting_sensor_2->property("measurementStatus").toString() == QStringLiteral("UNKNOWN"));
    assert(sorting_sensor_2->property("distanceCm").toInt() == -1);
    assert(sorting_sensor_2->text() == QStringLiteral("● S2 대기"));
    assert(sorting_sensor_2->styleSheet().contains(QStringLiteral("#6e6e6e")));
    bool has_gripper_title = false;
    bool has_transfer_state = false;
    for (const auto* label : panel.findChildren<QLabel*>()) {
        has_gripper_title = has_gripper_title || label->text() == QStringLiteral("그리퍼 이송");
        has_transfer_state = has_transfer_state || label->text() == QStringLiteral("컨베이어 사이 이송 중");
    }
    assert(has_gripper_title);
    assert(has_transfer_state);

    assert(
        state
            .applyEnvelope(SensorEnvelope("SORTING-SENSOR-2-CLEAR", "PI-SORTING-01", 2, QStringLiteral("CLEAR"), 42, 4))
            .applied);
    panel.setState(state);
    application.processEvents();
    assert(sorting_sensor_2->property("measurementStatus").toString() == QStringLiteral("CLEAR"));
    assert(sorting_sensor_2->property("distanceCm").toInt() == 42);
    assert(sorting_sensor_2->text() == QStringLiteral("● S2 없음 · 42 cm"));
    assert(sorting_sensor_2->styleSheet().contains(QStringLiteral("#89d185")));

    assert(state
               .applyEnvelope({
                   { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
                   { QStringLiteral("messageId"), QStringLiteral("SORTING-SENSOR-STALE") },
                   { QStringLiteral("messageType"), QStringLiteral("ERROR_OCCURRED") },
                   { QStringLiteral("sourceId"), QStringLiteral("PI-SORTING-01") },
                   { QStringLiteral("timestamp"), QStringLiteral("2026-07-23T01:00:04.500Z") },
                   { QStringLiteral("data"),
                     QJsonObject{
                         { QStringLiteral("errorCode"), QStringLiteral("ERR-HEALTH-SENSOR-STALE") },
                         { QStringLiteral("errorLevel"), QStringLiteral("WARNING") },
                         { QStringLiteral("currentState"), QStringLiteral("CONTROLLER_HEALTH") },
                         { QStringLiteral("message"), QStringLiteral("sensor stale") },
                     } },
               })
               .applied);
    panel.setState(state);
    application.processEvents();
    bool has_sensor_warning = false;
    bool has_sensor_warning_detail = false;
    for (const auto* label : panel.findChildren<QLabel*>()) {
        has_sensor_warning = has_sensor_warning || label->text() == QStringLiteral("센서 경고");
        has_sensor_warning_detail =
            has_sensor_warning_detail || label->text() == QStringLiteral("경고 · 센서 응답 지연");
    }
    assert(has_sensor_warning);
    assert(has_sensor_warning_detail);
    assert(sorting_sensor_2->property("measurementStatus").toString() == QStringLiteral("UNKNOWN"));
    assert(sorting_sensor_2->text() == QStringLiteral("● S2 대기"));
    assert(sorting_sensor_2->styleSheet().contains(QStringLiteral("#6e6e6e")));

    assert(
        state.applyEnvelope(SensorEnvelope("SORTING-SENSOR-2", "PI-SORTING-01", 2, QStringLiteral("DETECTED"), 11, 5))
            .applied);
    panel.setState(state);
    application.processEvents();
    bool has_detected_sensor = false;
    for (const auto* indicator : panel.findChildren<QLabel*>(QStringLiteral("sensorStatusIndicator"))) {
        if (indicator->property("sensorId").toInt() == 2 &&
            indicator->property("measurementStatus").toString() == QStringLiteral("DETECTED")) {
            has_detected_sensor = indicator->text() == QStringLiteral("● S2 감지 · 11 cm");
            assert(indicator->property("distanceCm").toInt() == 11);
            assert(indicator->toolTip().contains(QStringLiteral("11 cm")));
        }
    }
    assert(has_detected_sensor);
    assert(sorting_sensor_2->styleSheet().contains(QStringLiteral("#75beff")));

    assert(
        state.applyEnvelope(SensorEnvelope("SORTING-SENSOR-2-FAULT", "PI-SORTING-01", 2, QStringLiteral("FAULT"), 0, 6))
            .applied);
    panel.setState(state);
    application.processEvents();
    assert(sorting_sensor_2->property("measurementStatus").toString() == QStringLiteral("FAULT"));
    assert(sorting_sensor_2->text() == QStringLiteral("● S2 오류 · 0 cm"));
    assert(sorting_sensor_2->styleSheet().contains(QStringLiteral("#f14c4c")));

    assert(state
               .applyEnvelope(
                   SensorEnvelope("SORTING-SENSOR-2-RECOVERED", "PI-SORTING-01", 2, QStringLiteral("CLEAR"), 40, 7))
               .applied);
    panel.setState(state);
    application.processEvents();
    assert(sorting_sensor_2->property("measurementStatus").toString() == QStringLiteral("CLEAR"));
    assert(sorting_sensor_2->text() == QStringLiteral("● S2 없음 · 40 cm"));
    assert(sorting_sensor_2->styleSheet().contains(QStringLiteral("#89d185")));

    assert(state.applyEnvelope(DeviceEnvelope("VISION-WAITING", "PI-VISION-01", "WAITING_FOR_PRODUCT", "", 5)).applied);
    panel.setState(state);
    application.processEvents();
    bool has_product_waiting_state = false;
    for (const auto* label : panel.findChildren<QLabel*>()) {
        has_product_waiting_state = has_product_waiting_state || label->text() == QStringLiteral("상품 감지 대기");
    }
    assert(has_product_waiting_state);

    QString selected_target;
    QObject::connect(
        &panel, &logistics::control_center::OperationsDashboardPanel::controlTargetSelected,
        [&selected_target](const QString& target_device_id, const QString&) { selected_target = target_device_id; });
    QFrame* vision_card = nullptr;
    for (auto* card : panel.findChildren<QFrame*>(QStringLiteral("processUnitCard"))) {
        if (card->property("controlTargetDeviceId").toString() == QStringLiteral("PI-VISION-01")) {
            vision_card = card;
            break;
        }
    }
    assert(vision_card != nullptr);
    QMouseEvent select_vision(QEvent::MouseButtonRelease, QPointF(4, 4), QPointF(4, 4), QPointF(4, 4), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vision_card, &select_vision);
    assert(selected_target == QStringLiteral("PI-VISION-01"));
    assert(vision_card->property("selectedControlTarget").toBool());

    state.markMqttDisconnected(QDateTime::currentDateTimeUtc());
    panel.setState(state);
    panel.setMqttConnected(false);
    application.processEvents();

    const auto* live_status = panel.findChild<QLabel*>(QStringLiteral("dashboardLiveStatus"));
    assert(live_status != nullptr);
    assert(live_status->text() == QStringLiteral("● MQTT 연결 끊김"));
    assert(sorting_sensor_2->property("measurementStatus").toString() == QStringLiteral("UNKNOWN"));
    assert(sorting_sensor_2->styleSheet().contains(QStringLiteral("#6e6e6e")));
    int disconnected_status_count = 0;
    for (const auto* status : panel.findChildren<QLabel*>(QStringLiteral("processVisualStatus"))) {
        if (status->text() == QStringLiteral("연결 끊김")) {
            assert(status->styleSheet().contains(QStringLiteral("#777777")));
            ++disconnected_status_count;
        }
    }
    assert(disconnected_status_count == 5);
    return 0;
}
