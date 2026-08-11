#include "logistics/control_center/process_control_panel.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

#include "logistics/contracts/device.hpp"
#include "logistics/control_center/ui_dialog.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

QString IdleCommandStyle() {
    return QStringLiteral(
        "background-color:#1f1f1f;color:#cccccc;border:1px solid #4a4a4a;border-radius:4px;padding:4px;");
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

    const auto state = process.current_state.trimmed().toUpper();
    const auto role = logistics::contracts::DeviceRoleFromString(process.key.toStdString());
    const auto meaning = role.has_value() ? logistics::contracts::DeviceStateMeaningFor(*role, state.toStdString())
                                          : logistics::contracts::DeviceStateMeaning::kUnknown;
    if (meaning == logistics::contracts::DeviceStateMeaning::kEmergencyStop) {
        return ProcessControlPhase::EmergencyStop;
    }
    if (process.has_error || meaning == logistics::contracts::DeviceStateMeaning::kError) {
        return ProcessControlPhase::Error;
    }
    if (meaning == logistics::contracts::DeviceStateMeaning::kRecovery) {
        return ProcessControlPhase::Recovering;
    }
    if (meaning == logistics::contracts::DeviceStateMeaning::kStopped) {
        return ProcessControlPhase::Stopped;
    }
    if (state == QStringLiteral("WAITING_FOR_PRODUCT") ||
        meaning == logistics::contracts::DeviceStateMeaning::kWorking) {
        return ProcessControlPhase::Running;
    }
    if (meaning == logistics::contracts::DeviceStateMeaning::kIdle ||
        meaning == logistics::contracts::DeviceStateMeaning::kCompleted) {
        return ProcessControlPhase::Idle;
    }
    return ProcessControlPhase::Running;
}

}  // namespace

ProcessControlPanel::ProcessControlPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("processControlPanel"));
    setMinimumWidth(0);
    setMinimumHeight(44);
    setMaximumHeight(58);
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
        "QPushButton#emergencyStopButton:hover:enabled{background-color:#c42b1c;}"
        "#standardCommandGroup,#recoveryCommandGroup,#safetyCommandGroup{background:transparent;border:0;}"
        "QPushButton#startButton:disabled,QPushButton#stopButton:disabled,"
        "QPushButton#recoveryButton:disabled,QPushButton#emergencyStopButton:disabled{"
        "background:#202020;color:#626262;border:1px solid #2b2b2b;}");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("공정 제어"), this);
    title->setObjectName(QStringLiteral("processControlTitle"));
    title->setFixedWidth(64);
    title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    title->setStyleSheet("color:#f0f0f0;font-size:14px;font-weight:700;");
    target_label_ = new QLabel(QStringLiteral("제어 대상 · 전체 공정"), this);
    target_label_->setObjectName(QStringLiteral("processControlTarget"));
    target_label_->setFixedWidth(170);
    target_label_->setFixedHeight(28);
    target_label_->setAlignment(Qt::AlignCenter);
    target_label_->setStyleSheet(
        "background:#172534;color:#75beff;border:1px solid #285a7e;border-radius:4px;"
        "font-size:10px;font-weight:700;padding:4px 8px;");
    target_label_->setToolTip(
        QStringLiteral("상단 공정 카드를 클릭하여 대상을 변경합니다. 비상정지는 항상 전체 공정에 적용됩니다."));
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
    recovery_button_->setFixedWidth(78);
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

    auto* standard_group = new QFrame(this);
    standard_group->setObjectName(QStringLiteral("standardCommandGroup"));
    standard_group->setAccessibleName(QStringLiteral("일반 공정 제어"));
    auto* standard_layout = new QHBoxLayout(standard_group);
    standard_layout->setContentsMargins(0, 0, 0, 0);
    standard_layout->setSpacing(6);
    standard_layout->addWidget(start_button_);
    standard_layout->addWidget(stop_button_);

    auto* recovery_group = new QFrame(this);
    recovery_group->setObjectName(QStringLiteral("recoveryCommandGroup"));
    recovery_group->setAccessibleName(QStringLiteral("공정 복구 제어"));
    auto* recovery_layout = new QHBoxLayout(recovery_group);
    recovery_layout->setContentsMargins(0, 0, 0, 0);
    recovery_layout->addWidget(recovery_button_);

    auto* safety_group = new QFrame(this);
    safety_group->setObjectName(QStringLiteral("safetyCommandGroup"));
    safety_group->setAccessibleName(QStringLiteral("비상 안전 제어"));
    auto* safety_layout = new QHBoxLayout(safety_group);
    safety_layout->setContentsMargins(0, 0, 0, 0);
    safety_layout->addWidget(emergency_stop_button_);

    auto* safety_divider = new QFrame(this);
    safety_divider->setObjectName(QStringLiteral("safetyCommandDivider"));
    safety_divider->setFrameShape(QFrame::VLine);
    safety_divider->setFixedHeight(24);
    safety_divider->setStyleSheet("color:#3c3c3c;");

    layout->addWidget(title);
    layout->addWidget(target_label_);
    layout->addWidget(command_status_, 1);
    layout->addWidget(standard_group);
    layout->addWidget(recovery_group);
    layout->addWidget(safety_divider, 0, Qt::AlignVCenter);
    layout->addWidget(safety_group);

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
    const auto target_text = QStringLiteral("제어 대상 · %1").arg(selected_target_display_name_);
    target_label_->setText(
        target_label_->fontMetrics().elidedText(target_text, Qt::ElideRight, target_label_->width() - 18));
    target_label_->setToolTip(
        QStringLiteral("%1\n%2").arg(target_text, selectedTargetDeviceId() == QStringLiteral("SYSTEM")
                                                      ? QStringLiteral("시작·정지·복구는 전체 공정에 적용됩니다.")
                                                      : QStringLiteral("시작·정지·복구는 선택한 노드에 적용됩니다.")));
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

    if (!control_state_.isMqttConnected()) {
        const auto disconnected_hint = QStringLiteral("MQTT 연결 후 사용할 수 있습니다.");
        start_button_->setToolTip(disconnected_hint);
        stop_button_->setToolTip(disconnected_hint);
        recovery_button_->setToolTip(disconnected_hint);
        emergency_stop_button_->setToolTip(disconnected_hint);
        return;
    }
    start_button_->setToolTip(QStringLiteral("선택한 제어 대상의 공정을 시작합니다."));
    stop_button_->setToolTip(QStringLiteral("선택한 제어 대상의 공정을 정지합니다."));
    recovery_button_->setToolTip(sensor_warning ? QStringLiteral("센서 응답이 정상화되어야 복구할 수 있습니다.")
                                                : QStringLiteral("선택한 제어 대상의 오류 상태를 복구합니다."));
    emergency_stop_button_->setToolTip(QStringLiteral("전체 공정에 비상 정지 명령을 전송합니다."));
}

}  // namespace logistics::control_center
