#include "logistics/control_center/process_control_panel.hpp"

#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

QString CommandLabel(mqtt::ControlCommand command) {
    switch (command) {
        case mqtt::ControlCommand::kStart:
            return QStringLiteral("공정 시작");
        case mqtt::ControlCommand::kStop:
            return QStringLiteral("공정 정지");
        case mqtt::ControlCommand::kRestart:
            return QStringLiteral("공정 재시작");
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

}  // namespace

ProcessControlPanel::ProcessControlPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("processControlPanel"));
    setMinimumWidth(310);
    setMaximumWidth(360);
    setStyleSheet(
        "#processControlPanel{background-color:#0b1220;border:1px solid #243247;border-radius:12px;}"
        "QLabel{color:#e2e8f0;}"
        "QPushButton{min-height:42px;border:1px solid #334155;border-radius:8px;background-color:#1e293b;"
        "color:#f8fafc;font-size:14px;font-weight:700;padding:6px 12px;}"
        "QPushButton:hover:enabled{background-color:#334155;border-color:#475569;}"
        "QPushButton:pressed:enabled{background-color:#0f172a;}"
        "QPushButton:disabled{background-color:#111827;color:#64748b;border-color:#1e293b;}"
        "QPushButton#startButton{background-color:#075985;border-color:#0284c7;}"
        "QPushButton#startButton:hover:enabled{background-color:#0369a1;}"
        "QPushButton#stopButton{background-color:#3b2a0d;border-color:#92400e;color:#fde68a;}"
        "QPushButton#stopButton:hover:enabled{background-color:#4a310b;}"
        "QPushButton#emergencyStopButton{min-height:72px;background-color:#991b1b;color:#fff1f2;"
        "font-size:18px;border:1px solid #ef4444;}"
        "QPushButton#emergencyStopButton:hover:enabled{background-color:#b91c1c;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("공정 제어"), this);
    title->setStyleSheet("color:#f8fafc;font-size:20px;font-weight:800;");
    connection_hint_ = new QLabel(QStringLiteral("MQTT 연결 후 명령을 사용할 수 있습니다."), this);
    connection_hint_->setWordWrap(true);
    connection_hint_->setStyleSheet("color:#94a3b8;font-size:11px;");

    command_status_ = new QLabel(QStringLiteral("대기 중"), this);
    command_status_->setObjectName(QStringLiteral("commandStatus"));
    command_status_->setMinimumHeight(72);
    command_status_->setAlignment(Qt::AlignCenter);
    command_status_->setWordWrap(true);
    command_status_->setStyleSheet(
        "background-color: #1f2937; color: #d1d5db; border: 1px solid #374151; border-radius: 6px; padding: 8px;");

    start_button_ = new QPushButton(QStringLiteral("시작"), this);
    start_button_->setObjectName(QStringLiteral("startButton"));
    stop_button_ = new QPushButton(QStringLiteral("정지"), this);
    stop_button_->setObjectName(QStringLiteral("stopButton"));
    restart_button_ = new QPushButton(QStringLiteral("재시작"), this);
    restart_button_->setObjectName(QStringLiteral("restartButton"));
    emergency_stop_button_ = new QPushButton(QStringLiteral("비상정지\nEMERGENCY STOP"), this);
    emergency_stop_button_->setObjectName(QStringLiteral("emergencyStopButton"));

    connect(start_button_, &QPushButton::clicked, this,
            [this]() { requestCommand(mqtt::ControlCommand::kStart, QStringLiteral("공정을 시작하시겠습니까?")); });
    connect(stop_button_, &QPushButton::clicked, this,
            [this]() { requestCommand(mqtt::ControlCommand::kStop, QStringLiteral("공정을 정지하시겠습니까?")); });
    connect(restart_button_, &QPushButton::clicked, this,
            [this]() { requestCommand(mqtt::ControlCommand::kRestart, QStringLiteral("공정을 재시작하시겠습니까?")); });
    connect(emergency_stop_button_, &QPushButton::clicked, this,
            [this]() { emit commandRequested(mqtt::ControlCommand::kEmergencyStop); });

    layout->addWidget(title);
    layout->addWidget(connection_hint_);
    layout->addSpacing(8);
    layout->addWidget(command_status_);
    layout->addSpacing(8);
    layout->addWidget(start_button_);
    layout->addWidget(stop_button_);
    layout->addWidget(restart_button_);
    layout->addStretch();

    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet("color: #4b5563;");
    layout->addWidget(divider);
    layout->addWidget(emergency_stop_button_);

    updateButtonStates();
}

void ProcessControlPanel::setMqttConnected(bool connected) {
    control_state_.setMqttConnected(connected);
    connection_hint_->setText(connected ? QStringLiteral("중앙 서버 MQTT 연결됨")
                                        : QStringLiteral("MQTT 연결 후 명령을 사용할 수 있습니다."));
    if (connected && !control_state_.isCommandPending()) {
        command_status_->setText(QStringLiteral("대기 중"));
        command_status_->setStyleSheet(
            "background-color: #1f2937; color: #d1d5db; border: 1px solid #374151; border-radius: 6px; padding: 8px;");
    } else if (!connected) {
        command_status_->setText(QStringLiteral("MQTT 연결 끊김"));
        command_status_->setStyleSheet(
            "background-color: #1f2937; color: #fca5a5; border: 1px solid #7f1d1d; border-radius: 6px; padding: 8px;");
    }
    updateButtonStates();
}

void ProcessControlPanel::setCommandPending(mqtt::ControlCommand command) {
    control_state_.setCommandPending();
    command_status_->setText(QStringLiteral("%1 명령 전송됨\n응답 대기 중").arg(CommandLabel(command)));
    command_status_->setStyleSheet(
        "background-color: #1f2937; color: #fbbf24; border: 1px solid #92400e; border-radius: 6px; padding: 8px;");
    updateButtonStates();
}

void ProcessControlPanel::setCommandProgress(mqtt::ControlCommand command, mqtt::CommandResult result,
                                             const QString& detail) {
    control_state_.setCommandPending();
    auto text = QStringLiteral("%1\n%2").arg(CommandLabel(command), ResultLabel(result));
    if (!detail.isEmpty()) {
        text.append(QStringLiteral("\n%1").arg(detail));
    }
    command_status_->setText(text);
    command_status_->setStyleSheet(
        "background-color: #1f2937; color: #93c5fd; border: 1px solid #1d4ed8; border-radius: 6px; padding: 8px;");
    updateButtonStates();
}

void ProcessControlPanel::setCommandFinished(mqtt::ControlCommand command, mqtt::CommandResult result,
                                             const QString& detail) {
    control_state_.setCommandFinished();
    auto text = QStringLiteral("%1\n%2").arg(CommandLabel(command), ResultLabel(result));
    if (!detail.isEmpty()) {
        text.append(QStringLiteral("\n%1").arg(detail));
    }
    command_status_->setText(text);

    const bool succeeded = result == mqtt::CommandResult::kSuccess;
    command_status_->setStyleSheet(
        succeeded
            ? "background-color: #052e16; color: #86efac; border: 1px solid #15803d; border-radius: 6px; padding: 8px;"
            : "background-color: #450a0a; color: #fca5a5; border: 1px solid #b91c1c; border-radius: 6px; padding: "
              "8px;");
    updateButtonStates();
}

void ProcessControlPanel::requestCommand(mqtt::ControlCommand command, const QString& confirmation) {
    if (!control_state_.isMqttConnected() || control_state_.isCommandPending()) {
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("공정 제어 확인"), confirmation, QMessageBox::Yes,
                              QMessageBox::No) == QMessageBox::Yes) {
        emit commandRequested(command);
    }
}

void ProcessControlPanel::updateButtonStates() {
    start_button_->setEnabled(control_state_.normalCommandsEnabled());
    stop_button_->setEnabled(control_state_.normalCommandsEnabled());
    restart_button_->setEnabled(control_state_.normalCommandsEnabled());

    // Emergency stop stays available while a normal command is pending so it can always take priority.
    emergency_stop_button_->setEnabled(control_state_.emergencyStopEnabled());
}

}  // namespace logistics::control_center
