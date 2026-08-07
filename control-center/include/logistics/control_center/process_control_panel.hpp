#pragma once

#include <QHash>
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

    void setControlTarget(const QString& target_device_id, const QString& display_name);
    void setMqttConnected(bool connected);
    void setProcessState(OverallProcessState state);
    void setProcessStates(OverallProcessState overall_state, const QList<ProcessUnitStatus>& processes);
    void setCommandPending(logistics::contracts::mqtt::ControlCommand command);
    void setCommandProgress(logistics::contracts::mqtt::ControlCommand command,
                            logistics::contracts::mqtt::CommandResult result, const QString& detail = {});
    void setCommandFinished(logistics::contracts::mqtt::ControlCommand command,
                            logistics::contracts::mqtt::CommandResult result, const QString& detail = {});
    [[nodiscard]] QString selectedTargetDeviceId() const;

signals:
    void commandRequested(logistics::contracts::mqtt::ControlCommand command, const QString& target_device_id);

private:
    struct CommandPresentation {
        QString text;
        QString detail;
        QString style;
    };

    void requestCommand(logistics::contracts::mqtt::ControlCommand command, const QString& confirmation);
    [[nodiscard]] bool hasBlockingSensorWarning() const;
    void setCommandPresentation(const QString& target_device_id, const QString& text, const QString& style,
                                const QString& detail = {});
    void updateCommandPresentation();
    void updateTargetPresentation();
    void applySelectedTargetState();
    void updateButtonStates();

    QLabel* connection_hint_{ nullptr };
    QLabel* command_status_{ nullptr };
    QLabel* target_label_{ nullptr };
    QPushButton* start_button_{ nullptr };
    QPushButton* stop_button_{ nullptr };
    QPushButton* recovery_button_{ nullptr };
    QPushButton* emergency_stop_button_{ nullptr };
    ProcessControlState control_state_;
    OverallProcessState overall_state_{ OverallProcessState::Idle };
    QList<ProcessUnitStatus> process_statuses_;
    QString selected_target_device_id_{ QStringLiteral("SYSTEM") };
    QString selected_target_display_name_{ QStringLiteral("전체 공정") };
    QString command_target_device_id_;
    QHash<QString, CommandPresentation> command_presentations_;
};

}  // namespace logistics::control_center
