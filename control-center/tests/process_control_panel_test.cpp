#include "logistics/control_center/process_control_panel.hpp"

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <algorithm>
#include <cassert>
#include <vector>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::ProcessControlPanel panel;
    auto* start = panel.findChild<QPushButton*>(QStringLiteral("startButton"));
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("stopButton"));
    auto* recovery = panel.findChild<QPushButton*>(QStringLiteral("recoveryButton"));
    auto* emergency_stop = panel.findChild<QPushButton*>(QStringLiteral("emergencyStopButton"));
    auto* target_label = panel.findChild<QLabel*>(QStringLiteral("processControlTarget"));
    auto* command_status = panel.findChild<QLabel*>(QStringLiteral("commandStatus"));
    assert(start != nullptr);
    assert(stop != nullptr);
    assert(recovery != nullptr);
    assert(emergency_stop != nullptr);
    assert(target_label != nullptr);
    assert(command_status != nullptr);
    panel.resize(900, 92);
    panel.show();
    application.processEvents();
    assert(panel.minimumHeight() == 72);
    assert(panel.maximumHeight() <= 92);
    assert(panel.height() <= 92);
    assert(start->height() == 28);
    assert(stop->height() == 28);
    assert(recovery->height() == 28);
    assert(emergency_stop->height() == 32);
    for (const auto* widget : { static_cast<QWidget*>(start), static_cast<QWidget*>(stop),
                                static_cast<QWidget*>(recovery), static_cast<QWidget*>(emergency_stop),
                                static_cast<QWidget*>(target_label), static_cast<QWidget*>(command_status) }) {
        const QRect rect(widget->mapTo(&panel, QPoint{}), widget->size());
        assert(!rect.isEmpty());
        assert(panel.rect().contains(rect));
    }
    for (const auto* button : { start, stop, recovery, emergency_stop }) {
        assert(!button->text().contains(QLatin1Char('\n')));
    }
    std::vector<int> center_y;
    for (const auto* widget : { static_cast<QWidget*>(start), static_cast<QWidget*>(stop),
                                static_cast<QWidget*>(recovery), static_cast<QWidget*>(emergency_stop),
                                static_cast<QWidget*>(target_label), static_cast<QWidget*>(command_status) }) {
        center_y.push_back(widget->mapTo(&panel, widget->rect().center()).y());
    }
    std::ranges::sort(center_y);
    center_y.erase(std::unique(center_y.begin(), center_y.end()), center_y.end());
    assert(center_y.size() <= 2);
    assert(stop->text() == QStringLiteral("정지"));
    assert(recovery->text() == QStringLiteral("전체 복구"));
    assert(panel.findChild<QPushButton*>(QStringLiteral("restartButton")) == nullptr);
    assert(panel.findChild<QPushButton*>(QStringLiteral("initializeButton")) == nullptr);
    assert(!recovery->isEnabled());

    panel.setMqttConnected(true);
    assert(!start->isEnabled());
    assert(!recovery->isEnabled());

    panel.setControlTarget(QStringLiteral("SYSTEM"), QStringLiteral("전체 공정"));
    QList<logistics::control_center::ProcessUnitStatus> processes{
        {
            .key = QStringLiteral("vision"),
            .display_name = QStringLiteral("비전 처리"),
            .device_id = QStringLiteral("PI-VISION-01"),
            .connection_state = logistics::contracts::mqtt::ConnectionState::kOnline,
            .current_state = QStringLiteral("STOPPED"),
            .work_id = {},
            .destination = {},
            .work_completed = false,
            .error_code = {},
            .updated_at = QDateTime::currentDateTimeUtc(),
            .has_error = false,
            .sensors = {},
        },
        {
            .key = QStringLiteral("sorting"),
            .display_name = QStringLiteral("분류 컨베이어"),
            .device_id = QStringLiteral("PI-SORTING-01"),
            .connection_state = logistics::contracts::mqtt::ConnectionState::kOnline,
            .current_state = QStringLiteral("STOPPED"),
            .work_id = {},
            .destination = {},
            .work_completed = false,
            .error_code = {},
            .updated_at = QDateTime::currentDateTimeUtc(),
            .has_error = false,
            .sensors = {},
        },
    };
    panel.setProcessStates(logistics::control_center::OverallProcessState::Error, processes);
    panel.setControlTarget(QStringLiteral("PI-VISION-01"), QStringLiteral("비전 처리"));
    assert(panel.selectedTargetDeviceId() == QStringLiteral("PI-VISION-01"));
    assert(target_label->text() == QStringLiteral("제어 대상 · 비전 처리"));
    assert(start->isEnabled());
    assert(!recovery->isEnabled());

    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kStart);
    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kStart,
                             logistics::contracts::mqtt::CommandResult::kSuccess, QStringLiteral("비전 시작 완료"));
    assert(command_status->text() == QStringLiteral("공정 시작 · 완료"));
    assert(!command_status->text().contains(QStringLiteral("비전 시작 완료")));
    assert(!command_status->text().contains(QLatin1Char('\n')));
    assert(command_status->toolTip().contains(QStringLiteral("비전 시작 완료")));

    panel.setControlTarget(QStringLiteral("PI-SORTING-01"), QStringLiteral("분류 컨베이어"));
    assert(command_status->text() == QStringLiteral("대기 중"));
    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kStop);
    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kStop,
                             logistics::contracts::mqtt::CommandResult::kRejected, QStringLiteral("분류 정지 거부"));
    assert(command_status->text() == QStringLiteral("공정 정지 · NACK · 거부됨"));
    assert(!command_status->text().contains(QStringLiteral("분류 정지 거부")));

    panel.setControlTarget(QStringLiteral("PI-VISION-01"), QStringLiteral("비전 처리"));
    assert(command_status->text() == QStringLiteral("공정 시작 · 완료"));
    assert(command_status->toolTip().contains(QLatin1Char('\n')));

    panel.setMqttConnected(false);
    assert(command_status->text().contains(QStringLiteral("MQTT 연결 끊김")));
    assert(command_status->toolTip().contains(QStringLiteral("MQTT 연결 끊김")));
    assert(!command_status->toolTip().contains(QStringLiteral("비전 시작 완료")));
    panel.setControlTarget(QStringLiteral("PI-SORTING-01"), QStringLiteral("분류 컨베이어"));
    assert(command_status->text().contains(QStringLiteral("MQTT 연결 끊김")));
    assert(command_status->toolTip().contains(QStringLiteral("MQTT 연결 끊김")));
    assert(!command_status->toolTip().contains(QStringLiteral("비전 시작 완료")));
    panel.setControlTarget(QStringLiteral("PI-VISION-01"), QStringLiteral("비전 처리"));
    assert(command_status->text().contains(QStringLiteral("MQTT 연결 끊김")));
    assert(command_status->toolTip().contains(QStringLiteral("MQTT 연결 끊김")));
    assert(!command_status->toolTip().contains(QStringLiteral("비전 시작 완료")));
    panel.setMqttConnected(true);

    processes[0].current_state = QStringLiteral("WAITING_FOR_PRODUCT");
    panel.setProcessStates(logistics::control_center::OverallProcessState::Running, processes);
    assert(!start->isEnabled());
    assert(!recovery->isEnabled());

    processes[0].current_state = QStringLiteral("RECOVERY_READY");
    panel.setProcessStates(logistics::control_center::OverallProcessState::Recovery, processes);
    assert(start->isEnabled());
    assert(!recovery->isEnabled());

    QString emergency_target;
    QObject::connect(&panel, &logistics::control_center::ProcessControlPanel::commandRequested,
                     [&emergency_target](logistics::contracts::mqtt::ControlCommand command, const QString& target) {
                         if (command == logistics::contracts::mqtt::ControlCommand::kEmergencyStop) {
                             emergency_target = target;
                         }
                     });
    emergency_stop->click();
    assert(emergency_target == QStringLiteral("SYSTEM"));

    panel.setControlTarget(QStringLiteral("SYSTEM"), QStringLiteral("전체 공정"));
    panel.setProcessState(logistics::control_center::OverallProcessState::EmergencyStop);
    assert(recovery->isEnabled());

    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kRecovery);
    assert(!recovery->isEnabled());
    assert(emergency_stop->isEnabled());

    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kRecovery,
                             logistics::contracts::mqtt::CommandResult::kSuccess);
    assert(!recovery->isEnabled());
    assert(start->isEnabled());

    panel.setProcessState(logistics::control_center::OverallProcessState::EmergencyStop);
    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kRecovery);
    panel.setProcessState(logistics::control_center::OverallProcessState::Recovery);
    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kRecovery,
                             logistics::contracts::mqtt::CommandResult::kTimeout);
    assert(recovery->isEnabled());

    processes = {
        {
            .key = QString::fromLatin1(logistics::control_center::kInputProcessKey),
            .display_name = QStringLiteral("투입 컨베이어"),
            .device_id = QStringLiteral("PI-INPUT-01"),
            .connection_state = logistics::contracts::mqtt::ConnectionState::kOnline,
            .current_state = QStringLiteral("EMERGENCY_STOP"),
            .work_id = {},
            .destination = {},
            .work_completed = false,
            .error_code = {},
            .updated_at = QDateTime::currentDateTimeUtc(),
            .has_error = false,
            .sensors = {},
        },
        {
            .key = QString::fromLatin1(logistics::control_center::kSortingProcessKey),
            .display_name = QStringLiteral("분류 컨베이어"),
            .device_id = QStringLiteral("PI-SORTING-01"),
            .connection_state = logistics::contracts::mqtt::ConnectionState::kOnline,
            .current_state = QStringLiteral("EMERGENCY_STOP"),
            .work_id = {},
            .destination = {},
            .work_completed = false,
            .error_code = {},
            .updated_at = QDateTime::currentDateTimeUtc(),
            .has_error = false,
            .sensors = {},
        },
    };
    panel.setProcessStates(logistics::control_center::OverallProcessState::EmergencyStop, processes);
    panel.setControlTarget(QStringLiteral("PI-SORTING-01"), QStringLiteral("분류 컨베이어"));
    assert(target_label->text() == QStringLiteral("제어 대상 · 분류 컨베이어"));
    assert(recovery->text() == QStringLiteral("복구"));
    assert(recovery->isEnabled());

    QString recovery_target;
    QObject::connect(&panel, &logistics::control_center::ProcessControlPanel::commandRequested,
                     [&recovery_target](logistics::contracts::mqtt::ControlCommand command, const QString& target) {
                         if (command == logistics::contracts::mqtt::ControlCommand::kRecovery) {
                             recovery_target = target;
                         }
                     });
    QTimer::singleShot(0, []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        assert(dialog != nullptr);
        auto* accept = dialog->findChild<QPushButton*>(QStringLiteral("primaryDialogButton"));
        assert(accept != nullptr);
        accept->click();
    });
    recovery->click();
    assert(recovery_target == QStringLiteral("PI-SORTING-01"));

    processes[1].current_state = QStringLiteral("STOPPED");
    processes[1].error_code = QStringLiteral("ERR-HEALTH-SENSOR-STALE");
    processes[1].has_warning = true;
    panel.setProcessStates(logistics::control_center::OverallProcessState::Stopped, processes);
    assert(!start->isEnabled());
    assert(!recovery->isEnabled());
    assert(recovery->toolTip() == QStringLiteral("센서 응답이 정상화되어야 복구할 수 있습니다."));

    processes[1].error_code.clear();
    processes[1].has_warning = false;
    panel.setProcessStates(logistics::control_center::OverallProcessState::Stopped, processes);
    assert(start->isEnabled());
    return 0;
}
