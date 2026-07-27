#pragma once

#include <QWidget>

#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/control_center/operations_dashboard_state.hpp"
#include "logistics/control_center/process_control_state.hpp"

class QLabel;
class QPushButton;

namespace logistics::control_center {

class ProcessControlPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ProcessControlPanel(QWidget* parent = nullptr);

    void setMqttConnected(bool connected);
    void setProcessState(OverallProcessState state);
    void setCommandPending(logistics::contracts::mqtt::ControlCommand command);
    void setCommandProgress(logistics::contracts::mqtt::ControlCommand command,
                            logistics::contracts::mqtt::CommandResult result, const QString& detail = {});
    void setCommandFinished(logistics::contracts::mqtt::ControlCommand command,
                            logistics::contracts::mqtt::CommandResult result, const QString& detail = {});

signals:
    void commandRequested(logistics::contracts::mqtt::ControlCommand command);

private:
    void requestCommand(logistics::contracts::mqtt::ControlCommand command, const QString& confirmation);
    void updateButtonStates();

    QLabel* connection_hint_{ nullptr };
    QLabel* command_status_{ nullptr };
    QPushButton* start_button_{ nullptr };
    QPushButton* stop_button_{ nullptr };
    QPushButton* restart_button_{ nullptr };
    QPushButton* recovery_button_{ nullptr };
    QPushButton* initialize_button_{ nullptr };
    QPushButton* emergency_stop_button_{ nullptr };
    ProcessControlState control_state_;
};

}  // namespace logistics::control_center
