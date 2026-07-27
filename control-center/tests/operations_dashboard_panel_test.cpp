#include "logistics/control_center/operations_dashboard_panel.hpp"

#include <QApplication>
#include <QFrame>
#include <QJsonObject>
#include <QLabel>
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
    panel.resize(1280, 112);
    panel.setState(state);
    panel.setMqttConnected(true);
    panel.show();
    application.processEvents();

    assert(panel.minimumHeight() == 112);
    assert(panel.maximumHeight() == 112);
    assert(panel.findChildren<QFrame*>(QStringLiteral("processUnitCard")).size() == 5);
    bool has_gripper_title = false;
    bool has_transfer_state = false;
    for (const auto* label : panel.findChildren<QLabel*>()) {
        has_gripper_title = has_gripper_title || label->text() == QStringLiteral("그리퍼 이송");
        has_transfer_state = has_transfer_state || label->text() == QStringLiteral("컨베이어 사이 이송 중");
    }
    assert(has_gripper_title);
    assert(has_transfer_state);

    state.markMqttDisconnected(QDateTime::currentDateTimeUtc());
    panel.setState(state);
    panel.setMqttConnected(false);
    application.processEvents();

    const auto* live_status = panel.findChild<QLabel*>(QStringLiteral("dashboardLiveStatus"));
    assert(live_status != nullptr);
    assert(live_status->text() == QStringLiteral("● MQTT 연결 끊김"));
    int waiting_count = 0;
    int disconnected_count = 0;
    for (const auto* label : panel.findChildren<QLabel*>()) {
        waiting_count += label->text() == QStringLiteral("수신 대기") ? 1 : 0;
        disconnected_count += label->text() == QStringLiteral("연결 끊김") ? 1 : 0;
    }
    assert(waiting_count == 5);
    assert(disconnected_count == 5);
    return 0;
}
