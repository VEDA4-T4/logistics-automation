#include "logistics/control_center/process_control_panel.hpp"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <cassert>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::ProcessControlPanel panel;
    auto* start = panel.findChild<QPushButton*>(QStringLiteral("startButton"));
    auto* stop = panel.findChild<QPushButton*>(QStringLiteral("stopButton"));
    auto* recovery = panel.findChild<QPushButton*>(QStringLiteral("recoveryButton"));
    auto* emergency_stop = panel.findChild<QPushButton*>(QStringLiteral("emergencyStopButton"));
    auto* target_label = panel.findChild<QLabel*>(QStringLiteral("processControlTarget"));
    assert(start != nullptr);
    assert(stop != nullptr);
    assert(recovery != nullptr);
    assert(emergency_stop != nullptr);
    assert(target_label != nullptr);
    assert(stop->text() == QStringLiteral("정지"));
    assert(recovery->text() == QStringLiteral("복구"));
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
            .error_code = {},
            .updated_at = QDateTime::currentDateTimeUtc(),
            .has_error = false,
        },
    };
    panel.setProcessStates(logistics::control_center::OverallProcessState::Error, processes);
    panel.setControlTarget(QStringLiteral("PI-VISION-01"), QStringLiteral("비전 처리"));
    assert(panel.selectedTargetDeviceId() == QStringLiteral("PI-VISION-01"));
    assert(target_label->text() == QStringLiteral("제어 대상 · 비전 처리"));
    assert(start->isEnabled());
    assert(!recovery->isEnabled());

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
    return 0;
}
