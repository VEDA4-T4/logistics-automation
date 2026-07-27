#include "logistics/control_center/process_control_panel.hpp"

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <cassert>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::ProcessControlPanel panel;
    auto* start = panel.findChild<QPushButton*>(QStringLiteral("startButton"));
    auto* recovery = panel.findChild<QPushButton*>(QStringLiteral("recoveryButton"));
    auto* initialize = panel.findChild<QPushButton*>(QStringLiteral("initializeButton"));
    auto* emergency_stop = panel.findChild<QPushButton*>(QStringLiteral("emergencyStopButton"));
    auto* target_selector = panel.findChild<QComboBox*>(QStringLiteral("processTargetSelector"));
    assert(start != nullptr);
    assert(recovery != nullptr);
    assert(initialize != nullptr);
    assert(emergency_stop != nullptr);
    assert(target_selector != nullptr);
    assert(recovery->text() == QStringLiteral("복구"));
    assert(initialize->text() == QStringLiteral("초기화"));
    assert(!recovery->isEnabled());
    assert(!initialize->isEnabled());

    panel.setMqttConnected(true);
    assert(!start->isEnabled());
    assert(!recovery->isEnabled());
    assert(!initialize->isEnabled());

    panel.configureTargets(QStringLiteral("SYSTEM"), {
                                                         { QStringLiteral("vision"), QStringLiteral("비전 처리"),
                                                           QStringLiteral("PI-VISION-01") },
                                                     });
    const QList<logistics::control_center::ProcessUnitStatus> processes{
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
    target_selector->setCurrentIndex(target_selector->findData(QStringLiteral("PI-VISION-01")));
    assert(panel.selectedTargetDeviceId() == QStringLiteral("PI-VISION-01"));
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

    target_selector->setCurrentIndex(target_selector->findData(QStringLiteral("SYSTEM")));
    panel.setProcessState(logistics::control_center::OverallProcessState::EmergencyStop);
    assert(recovery->isEnabled());
    assert(!initialize->isEnabled());

    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kRecovery);
    assert(!recovery->isEnabled());
    assert(!initialize->isEnabled());
    assert(emergency_stop->isEnabled());
    assert(!target_selector->isEnabled());

    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kRecovery,
                             logistics::contracts::mqtt::CommandResult::kSuccess);
    assert(target_selector->isEnabled());
    assert(!recovery->isEnabled());
    assert(initialize->isEnabled());
    panel.setProcessState(logistics::control_center::OverallProcessState::Recovery);
    assert(!recovery->isEnabled());
    assert(initialize->isEnabled());

    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kInitialize);
    assert(!start->isEnabled());
    assert(!initialize->isEnabled());
    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kInitialize,
                             logistics::contracts::mqtt::CommandResult::kSuccess);
    assert(start->isEnabled());
    assert(!recovery->isEnabled());
    assert(!initialize->isEnabled());

    panel.setProcessState(logistics::control_center::OverallProcessState::EmergencyStop);
    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kRecovery);
    panel.setProcessState(logistics::control_center::OverallProcessState::Recovery);
    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kRecovery,
                             logistics::contracts::mqtt::CommandResult::kTimeout);
    assert(recovery->isEnabled());
    assert(!initialize->isEnabled());
    return 0;
}
