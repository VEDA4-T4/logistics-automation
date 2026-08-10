#include "logistics/control_center/process_control_panel.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

#include "logistics/control_center/ui_dialog.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

QString IdleCommandStyle() {
    return QStringLiteral(
        "background-color:#1f1f1f;color:#cccccc;border:1px solid #2b2b2b;border-radius:4px;padding:4px;");
}

QString PendingCommandStyle() {
    return QStringLiteral(
        "background-color:#3a3000;color:#cca700;border:1px solid #6b5d00;border-radius:4px;padding:4px;");
}

QString ProgressCommandStyle() {
    return QStringLiteral(
        "background-color:#182c3a;color:#4daafc;border:1px solid #264f78;border-radius:4px;padding:4px;");
}

QString SuccessCommandStyle() {
    return QStringLiteral(
        "background-color:#1f3325;color:#89d185;border:1px solid #385a40;border-radius:4px;padding:4px;");
}

QString FailureCommandStyle() {
    return QStringLiteral(
        "background-color:#3b1f22;color:#f14c4c;border:1px solid #6e2b2f;border-radius:4px;padding:4px;");
}

QString CommandLabel(mqtt::ControlCommand command) {
    switch (command) {
        case mqtt::ControlCommand::kStart:
            return QStringLiteral("공정 시작");
        case mqtt::ControlCommand::kExecute:
            return QStringLiteral("작업 실행");
        case mqtt::ControlCommand::kStop:
            return QStringLiteral("공정 정지");
        case mqtt::ControlCommand::kRestart:
            return QStringLiteral("공정 재시작");
        case mqtt::ControlCommand::kRecovery:
            return QStringLiteral("공정 복구");
        case mqtt::ControlCommand::kInitialize:
            return QStringLiteral("공정 초기화");
        case mqtt::ControlCommand::kEmergencyStop:
            return QStringLiteral("비상정지");
        default:
            return QStringLiteral("공정 명령");
    }
}

QString ResultLabel(mqtt::CommandResult result) {
    switch (result) {
        case mqtt::CommandResult::kReceived:
            return QStringLiteral("ACK · 접수됨");
        case mqtt::CommandResult::kProcessing:
            return QStringLiteral("ACK · 처리 중");
        case mqtt::CommandResult::kSuccess:
            return QStringLiteral("완료");
        case mqtt::CommandResult::kFailed:
            return QStringLiteral("NACK · 실패");
        case mqtt::CommandResult::kRejected:
            return QStringLiteral("NACK · 거부됨");
        case mqtt::CommandResult::kTimeout:
            return QStringLiteral("NACK · 응답 시간 초과");
        case mqtt::CommandResult::kDuplicated:
            return QStringLiteral("중복 요청");
        case mqtt::CommandResult::kUnknown:
            break;
    }
    return QStringLiteral("알 수 없는 결과");
}

ProcessControlPhase PhaseForProcess(const ProcessUnitStatus& process) {
    if (!process.updated_at.isValid() || process.connection_state == mqtt::ConnectionState::kUnknown ||
        process.connection_state == mqtt::ConnectionState::kOffline) {
        return ProcessControlPhase::Unknown;
    }
    if (process.has_error) {
        return ProcessControlPhase::Error;
    }

    const auto state = process.current_state.trimmed().toUpper();
    if (state == QStringLiteral("EMERGENCY_STOP") || state == QStringLiteral("ESTOP")) {
        return ProcessControlPhase::EmergencyStop;
    }
    if (state == QStringLiteral("RECOVERY") || state == QStringLiteral("RECOVERING")) {
        return ProcessControlPhase::Recovering;
    }
    if (state == QStringLiteral("RECOVERY_READY")) {
        return ProcessControlPhase::Stopped;
    }
    if (state == QStringLiteral("STOPPED")) {
        return ProcessControlPhase::Stopped;
    }
    if (state == QStringLiteral("ERROR") || state.endsWith(QStringLiteral("_ERROR"))) {
        return ProcessControlPhase::Error;
    }
    if (state == QStringLiteral("WAITING_FOR_PRODUCT")) {
        return ProcessControlPhase::Running;
    }
    if (state == QStringLiteral("IDLE") || state == QStringLiteral("READY") || state == QStringLiteral("WAITING") ||
        state == QStringLiteral("ONLINE") || state == QStringLiteral("COMPLETED")) {
        return ProcessControlPhase::Idle;
    }
    return ProcessControlPhase::Running;
}

}  // namespace

ProcessControlPanel::ProcessControlPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("processControlPanel"));
    setMinimumWidth(0);
    setMinimumHeight(72);
    setMaximumHeight(92);
    setStyleSheet(
        "#processControlPanel{background-color:#181818;border:1px solid #303030;border-radius:6px;}"
        "QPushButton{font-size:12px;font-weight:600;padding:0 8px;}"
        "QPushButton:pressed:enabled{background-color:#252526;}"
        "QPushButton#startButton{background-color:#0e639c;border-color:#1177bb;}"
        "QPushButton#startButton:hover:enabled{background-color:#1177bb;}"
        "QPushButton#stopButton{color:#cca700;}"
        "QPushButton#recoveryButton{color:#75beff;}"
        "QPushButton#emergencyStopButton{background-color:#a1260d;color:#ffffff;"
        "font-size:13px;border:1px solid #c42b1c;}"
        "QPushButton#emergencyStopButton:hover:enabled{background-color:#c42b1c;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    auto* title = new QLabel(QStringLiteral("공정 제어"), this);
    title->setStyleSheet("color:#f0f0f0;font-size:14px;font-weight:700;");
    target_label_ = new QLabel(QStringLiteral("제어 대상 · 전체 공정"), this);
    target_label_->setObjectName(QStringLiteral("processControlTarget"));
    target_label_->setStyleSheet(
        "background:#172534;color:#75beff;border:1px solid #285a7e;border-radius:4px;"
        "font-size:10px;font-weight:700;padding:4px 8px;");
    target_label_->setToolTip(
        QStringLiteral("상단 공정 카드를 클릭하여 대상을 변경합니다. 비상정지는 항상 전체 공정에 적용됩니다."));
    connection_hint_ = new QLabel(QStringLiteral("MQTT 연결 후 명령을 사용할 수 있습니다."), this);
    connection_hint_->setWordWrap(false);
    connection_hint_->setStyleSheet("color:#9d9d9d;font-size:10px;");

    command_status_ = new QLabel(QStringLiteral("대기 중"), this);
    command_status_->setObjectName(QStringLiteral("commandStatus"));
    command_status_->setFixedHeight(28);
    command_status_->setAlignment(Qt::AlignCenter);
    command_status_->setWordWrap(false);
    command_status_->setStyleSheet(IdleCommandStyle());

    start_button_ = new QPushButton(QStringLiteral("시작"), this);
    start_button_->setObjectName(QStringLiteral("startButton"));
    stop_button_ = new QPushButton(QStringLiteral("정지"), this);
    stop_button_->setObjectName(QStringLiteral("stopButton"));
    recovery_button_ = new QPushButton(QStringLiteral("복구"), this);
    recovery_button_->setObjectName(QStringLiteral("recoveryButton"));
    emergency_stop_button_ = new QPushButton(QStringLiteral("비상 정지"), this);
    emergency_stop_button_->setObjectName(QStringLiteral("emergencyStopButton"));
    for (auto* button : { start_button_, stop_button_, recovery_button_ }) {
        button->setFixedHeight(28);
    }
    emergency_stop_button_->setFixedHeight(32);

    connect(start_button_, &QPushButton::clicked, this,
            [this]() { requestCommand(mqtt::ControlCommand::kStart, QStringLiteral("공정을 시작하시겠습니까?")); });
    connect(stop_button_, &QPushButton::clicked, this,
            [this]() { requestCommand(mqtt::ControlCommand::kStop, QStringLiteral("공정을 정지하시겠습니까?")); });
    connect(recovery_button_, &QPushButton::clicked, this, [this]() {
        requestCommand(mqtt::ControlCommand::kRecovery, QStringLiteral("선택한 대상의 공정 복구를 시작하시겠습니까?"));
    });
    connect(emergency_stop_button_, &QPushButton::clicked, this, [this]() {
        command_target_device_id_ = QStringLiteral("SYSTEM");
        emit commandRequested(mqtt::ControlCommand::kEmergencyStop, command_target_device_id_);
    });

    auto* information_row = new QHBoxLayout();
    information_row->setContentsMargins(0, 0, 0, 0);
    information_row->setSpacing(8);
    information_row->addWidget(title);
    information_row->addWidget(target_label_);
    information_row->addWidget(command_status_, 1);
    information_row->addWidget(connection_hint_);

    auto* command_row = new QHBoxLayout();
    command_row->setContentsMargins(0, 0, 0, 0);
    command_row->setSpacing(8);
    command_row->addStretch(1);
    command_row->addWidget(start_button_);
    command_row->addWidget(stop_button_);
    command_row->addWidget(recovery_button_);

    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::VLine);
    divider->setStyleSheet("color:#303030;");
    command_row->addWidget(divider);
    command_row->addWidget(emergency_stop_button_);
    layout->addLayout(information_row);
    layout->addLayout(command_row);

    updateTargetPresentation();
    updateButtonStates();
}

void ProcessControlPanel::setControlTarget(const QString& target_device_id, const QString& display_name) {
    selected_target_device_id_ = target_device_id.isEmpty() ? QStringLiteral("SYSTEM") : target_device_id;
    selected_target_display_name_ = display_name.isEmpty() ? selected_target_device_id_ : display_name;
    updateTargetPresentation();
    updateCommandPresentation();
    applySelectedTargetState();
}

void ProcessControlPanel::setMqttConnected(bool connected) {
    control_state_.setMqttConnected(connected);
    connection_hint_->setText(connected ? QStringLiteral("중앙 서버 MQTT 연결됨")
                                        : QStringLiteral("MQTT 연결 후 명령을 사용할 수 있습니다."));
    if (connected) {
        updateCommandPresentation();
    } else if (!connected) {
        if (!command_target_device_id_.isEmpty()) {
            setCommandPresentation(command_target_device_id_, QStringLiteral("MQTT 연결 끊김\n명령 응답 확인 중단"),
                                   FailureCommandStyle());
        }
        command_target_device_id_.clear();
        command_status_->setText(QStringLiteral("MQTT 연결 끊김"));
        command_status_->setToolTip(QStringLiteral("MQTT 연결 끊김"));
        command_status_->setStyleSheet(FailureCommandStyle());
    }
    updateButtonStates();
}

void ProcessControlPanel::setProcessState(const OverallProcessState state) {
    overall_state_ = state;
    if (selectedTargetDeviceId() != QStringLiteral("SYSTEM")) {
        applySelectedTargetState();
        return;
    }
    switch (state) {
        case OverallProcessState::Idle:
            control_state_.setPhase(ProcessControlPhase::Idle);
            break;
        case OverallProcessState::Running:
        case OverallProcessState::Completed:
            control_state_.setPhase(ProcessControlPhase::Running);
            break;
        case OverallProcessState::Stopped:
            control_state_.setPhase(ProcessControlPhase::Stopped);
            break;
        case OverallProcessState::Error:
            control_state_.setPhase(ProcessControlPhase::Error);
            break;
        case OverallProcessState::EmergencyStop:
            control_state_.setPhase(ProcessControlPhase::EmergencyStop);
            break;
        case OverallProcessState::Recovery:
            control_state_.setPhase(ProcessControlPhase::Recovering);
            break;
    }
    updateButtonStates();
}

void ProcessControlPanel::setProcessStates(const OverallProcessState overall_state,
                                           const QList<ProcessUnitStatus>& processes) {
    overall_state_ = overall_state;
    process_statuses_ = processes;
    updateTargetPresentation();
    applySelectedTargetState();
}

void ProcessControlPanel::setCommandPending(mqtt::ControlCommand command) {
    if (command_target_device_id_.isEmpty()) {
        command_target_device_id_ = selectedTargetDeviceId();
    }
    control_state_.setCommandPending();
    setCommandPresentation(command_target_device_id_, QStringLiteral("%1 · 응답 대기 중").arg(CommandLabel(command)),
                           PendingCommandStyle());
    updateButtonStates();
}

void ProcessControlPanel::setCommandProgress(mqtt::ControlCommand command, mqtt::CommandResult result,
                                             const QString& detail) {
    control_state_.setCommandPending();
    const auto text = QStringLiteral("%1 · %2").arg(CommandLabel(command), ResultLabel(result));
    const auto target = command_target_device_id_.isEmpty() ? selectedTargetDeviceId() : command_target_device_id_;
    setCommandPresentation(target, text, ProgressCommandStyle(), detail);
    updateButtonStates();
}

void ProcessControlPanel::setCommandFinished(mqtt::ControlCommand command, mqtt::CommandResult result,
                                             const QString& detail) {
    control_state_.setCommandFinished();
    const auto text = QStringLiteral("%1 · %2").arg(CommandLabel(command), ResultLabel(result));
    const bool succeeded = result == mqtt::CommandResult::kSuccess;
    const auto target = command_target_device_id_.isEmpty() ? selectedTargetDeviceId() : command_target_device_id_;
    setCommandPresentation(target, text, succeeded ? SuccessCommandStyle() : FailureCommandStyle(), detail);
    const bool applies_to_selected_target =
        command_target_device_id_.isEmpty() || command_target_device_id_ == selectedTargetDeviceId();
    if (succeeded && applies_to_selected_target) {
        switch (command) {
            case mqtt::ControlCommand::kStart:
            case mqtt::ControlCommand::kRestart:
                control_state_.setPhase(ProcessControlPhase::Running);
                break;
            case mqtt::ControlCommand::kStop:
                control_state_.setPhase(ProcessControlPhase::Stopped);
                break;
            case mqtt::ControlCommand::kEmergencyStop:
                control_state_.setPhase(ProcessControlPhase::EmergencyStop);
                break;
            case mqtt::ControlCommand::kRecovery:
            case mqtt::ControlCommand::kInitialize:
                control_state_.setPhase(ProcessControlPhase::Stopped);
                break;
            case mqtt::ControlCommand::kStatusRequest:
            case mqtt::ControlCommand::kExecute:
            case mqtt::ControlCommand::kDestinationSet:
            case mqtt::ControlCommand::kUnknown:
                break;
        }
    } else if (!applies_to_selected_target) {
        applySelectedTargetState();
    }
    command_target_device_id_.clear();
    updateCommandPresentation();
    updateButtonStates();
}

void ProcessControlPanel::requestCommand(mqtt::ControlCommand command, const QString& confirmation) {
    if (!control_state_.isMqttConnected() || control_state_.isCommandPending()) {
        return;
    }

    const auto target_device_id = selectedTargetDeviceId();
    const auto target_description = QStringLiteral("%1 (%2)").arg(selected_target_display_name_, target_device_id);
    if (ShowConfirmationDialog(this, QStringLiteral("공정 제어 확인"),
                               QStringLiteral("%1\n\n대상: %2").arg(confirmation, target_description),
                               CommandLabel(command))) {
        command_target_device_id_ = target_device_id;
        emit commandRequested(command, command_target_device_id_);
    }
}

QString ProcessControlPanel::selectedTargetDeviceId() const {
    return selected_target_device_id_;
}

bool ProcessControlPanel::hasBlockingSensorWarning() const {
    const auto target_device_id = selectedTargetDeviceId();
    const auto process = std::find_if(
        process_statuses_.cbegin(), process_statuses_.cend(),
        [&target_device_id](const ProcessUnitStatus& candidate) { return candidate.device_id == target_device_id; });
    return process != process_statuses_.cend() && process->has_warning && IsSensorStaleErrorCode(process->error_code);
}

void ProcessControlPanel::setCommandPresentation(const QString& target_device_id, const QString& text,
                                                 const QString& style, const QString& detail) {
    const auto target = target_device_id.isEmpty() ? QStringLiteral("SYSTEM") : target_device_id;
    const CommandPresentation presentation{ .text = text, .detail = detail, .style = style };
    if (target == QStringLiteral("SYSTEM") || target == QStringLiteral("ALL")) {
        command_presentations_.insert(QStringLiteral("SYSTEM"), presentation);
        command_presentations_.insert(selectedTargetDeviceId(), presentation);
        for (const auto& process : process_statuses_) {
            command_presentations_.insert(process.device_id, presentation);
        }
    } else {
        command_presentations_.insert(target, presentation);
    }
    updateCommandPresentation();
}

void ProcessControlPanel::updateCommandPresentation() {
    if (!control_state_.isMqttConnected()) {
        return;
    }
    const auto presentation = command_presentations_.constFind(selectedTargetDeviceId());
    if (presentation == command_presentations_.cend()) {
        command_status_->setText(QStringLiteral("대기 중"));
        command_status_->setToolTip({});
        command_status_->setStyleSheet(IdleCommandStyle());
        return;
    }
    command_status_->setText(presentation->text);
    command_status_->setToolTip(presentation->detail.isEmpty()
                                    ? presentation->text
                                    : QStringLiteral("%1\n%2").arg(presentation->text, presentation->detail));
    command_status_->setStyleSheet(presentation->style);
}

void ProcessControlPanel::updateTargetPresentation() {
    target_label_->setText(QStringLiteral("제어 대상 · %1").arg(selected_target_display_name_));
    target_label_->setToolTip(selectedTargetDeviceId() == QStringLiteral("SYSTEM")
                                  ? QStringLiteral("시작·정지·복구는 전체 공정에 적용됩니다.")
                                  : QStringLiteral("시작·정지·복구는 선택한 노드에 적용됩니다."));
    recovery_button_->setText(selectedTargetDeviceId() == QStringLiteral("SYSTEM") ? QStringLiteral("전체 복구")
                                                                                   : QStringLiteral("복구"));
    recovery_button_->setToolTip(
        hasBlockingSensorWarning() ? QStringLiteral("센서 응답이 정상화되어야 복구할 수 있습니다.") : QString{});
}

void ProcessControlPanel::applySelectedTargetState() {
    const auto target_device_id = selectedTargetDeviceId();
    if (target_device_id == QStringLiteral("SYSTEM")) {
        setProcessState(overall_state_);
        return;
    }

    const auto process = std::find_if(
        process_statuses_.cbegin(), process_statuses_.cend(),
        [&target_device_id](const ProcessUnitStatus& candidate) { return candidate.device_id == target_device_id; });
    if (process == process_statuses_.cend()) {
        control_state_.setPhase(ProcessControlPhase::Unknown);
    } else {
        control_state_.setPhase(PhaseForProcess(*process));
    }
    updateButtonStates();
}

void ProcessControlPanel::updateButtonStates() {
    const bool sensor_warning = hasBlockingSensorWarning();
    start_button_->setEnabled(control_state_.startEnabled() && !sensor_warning);
    stop_button_->setEnabled(control_state_.stopEnabled());
    recovery_button_->setEnabled(control_state_.recoveryEnabled() && !sensor_warning);

    // Emergency stop stays available while a normal command is pending so it can always take priority.
    emergency_stop_button_->setEnabled(control_state_.emergencyStopEnabled());
}

}  // namespace logistics::control_center
