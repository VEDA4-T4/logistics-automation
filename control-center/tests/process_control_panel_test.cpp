#include "logistics/control_center/process_control_panel.hpp"

#include <QApplication>
#include <QPushButton>
#include <cassert>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::ProcessControlPanel panel;
    auto* recovery = panel.findChild<QPushButton*>(QStringLiteral("recoveryButton"));
    auto* initialize = panel.findChild<QPushButton*>(QStringLiteral("initializeButton"));
    auto* emergency_stop = panel.findChild<QPushButton*>(QStringLiteral("emergencyStopButton"));
    assert(recovery != nullptr);
    assert(initialize != nullptr);
    assert(emergency_stop != nullptr);
    assert(recovery->text() == QStringLiteral("복구"));
    assert(initialize->text() == QStringLiteral("초기화"));
    assert(!recovery->isEnabled());
    assert(!initialize->isEnabled());

    panel.setMqttConnected(true);
    assert(recovery->isEnabled());
    assert(initialize->isEnabled());

    panel.setCommandPending(logistics::contracts::mqtt::ControlCommand::kRecovery);
    assert(!recovery->isEnabled());
    assert(!initialize->isEnabled());
    assert(emergency_stop->isEnabled());

    panel.setCommandFinished(logistics::contracts::mqtt::ControlCommand::kRecovery,
                             logistics::contracts::mqtt::CommandResult::kSuccess);
    assert(recovery->isEnabled());
    assert(initialize->isEnabled());
    return 0;
}
