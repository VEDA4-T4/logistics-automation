#include "logistics/control_center/operations_dashboard_panel.hpp"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

struct StatusPresentation {
    QString text;
    QString background;
    QString foreground;
    QString border;
};

QString PillStyle(const StatusPresentation& presentation) {
    return QStringLiteral(
               "background:%1;color:%2;border:1px solid %3;border-radius:4px;"
               "font-size:9px;font-weight:700;padding:3px 6px;")
        .arg(presentation.background, presentation.foreground, presentation.border);
}

StatusPresentation OverallPresentation(OverallProcessState state) {
    switch (state) {
        case OverallProcessState::Idle:
            return { QStringLiteral("대기"), QStringLiteral("#252526"), QStringLiteral("#cccccc"),
                     QStringLiteral("#3c3c3c") };
        case OverallProcessState::Running:
            return { QStringLiteral("가동 중"), QStringLiteral("#17324a"), QStringLiteral("#75beff"),
                     QStringLiteral("#285a7e") };
        case OverallProcessState::Completed:
            return { QStringLiteral("완료"), QStringLiteral("#1f3325"), QStringLiteral("#89d185"),
                     QStringLiteral("#385a40") };
        case OverallProcessState::Stopped:
            return { QStringLiteral("정지"), QStringLiteral("#3a3000"), QStringLiteral("#cca700"),
                     QStringLiteral("#6b5d00") };
        case OverallProcessState::Error:
            return { QStringLiteral("오류"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                     QStringLiteral("#6e2b2f") };
        case OverallProcessState::EmergencyStop:
            return { QStringLiteral("비상정지"), QStringLiteral("#521b20"), QStringLiteral("#ff7b72"),
                     QStringLiteral("#a1262f") };
        case OverallProcessState::Recovery:
            return { QStringLiteral("복구 중"), QStringLiteral("#17324a"), QStringLiteral("#75beff"),
                     QStringLiteral("#285a7e") };
    }
    return {};
}

StatusPresentation ProcessPresentation(const ProcessUnitStatus& process) {
    if (process.has_error) {
        return { QStringLiteral("오류"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                 QStringLiteral("#6e2b2f") };
    }
    switch (process.connection_state) {
        case mqtt::ConnectionState::kOnline:
            return { QStringLiteral("온라인"), QStringLiteral("#1f3325"), QStringLiteral("#89d185"),
                     QStringLiteral("#385a40") };
        case mqtt::ConnectionState::kDelayed:
            return { QStringLiteral("응답 지연"), QStringLiteral("#3a3000"), QStringLiteral("#cca700"),
                     QStringLiteral("#6b5d00") };
        case mqtt::ConnectionState::kReconnecting:
            return { QStringLiteral("재연결 중"), QStringLiteral("#3a2a20"), QStringLiteral("#ce9178"),
                     QStringLiteral("#6b4938") };
        case mqtt::ConnectionState::kOffline:
            return { QStringLiteral("오프라인"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                     QStringLiteral("#6e2b2f") };
        case mqtt::ConnectionState::kRtspError:
            return { QStringLiteral("RTSP 오류"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                     QStringLiteral("#6e2b2f") };
        case mqtt::ConnectionState::kMqttError:
            return { QStringLiteral("MQTT 오류"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                     QStringLiteral("#6e2b2f") };
        case mqtt::ConnectionState::kMqttAuthError:
            return { QStringLiteral("인증 오류"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                     QStringLiteral("#6e2b2f") };
        case mqtt::ConnectionState::kTlsError:
            return { QStringLiteral("TLS 오류"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                     QStringLiteral("#6e2b2f") };
        case mqtt::ConnectionState::kUartError:
            return { QStringLiteral("UART 오류"), QStringLiteral("#3b1f22"), QStringLiteral("#f14c4c"),
                     QStringLiteral("#6e2b2f") };
        case mqtt::ConnectionState::kUnknown:
            break;
    }
    return { QStringLiteral("수신 대기"), QStringLiteral("#252526"), QStringLiteral("#9d9d9d"),
             QStringLiteral("#3c3c3c") };
}

QString UpdatedAtText(const QDateTime& updated_at) {
    if (!updated_at.isValid()) {
        return QStringLiteral("갱신 기록 없음");
    }
    const auto elapsed = qMax<qint64>(0, updated_at.secsTo(QDateTime::currentDateTimeUtc()));
    QString relative;
    if (elapsed < 5) {
        relative = QStringLiteral("방금");
    } else if (elapsed < 60) {
        relative = QStringLiteral("%1초 전").arg(elapsed);
    } else if (elapsed < 3600) {
        relative = QStringLiteral("%1분 전").arg(elapsed / 60);
    } else {
        relative = QStringLiteral("%1시간 전").arg(elapsed / 3600);
    }
    return QStringLiteral("%1 · %2").arg(updated_at.toLocalTime().toString(QStringLiteral("HH:mm:ss")), relative);
}

QString CurrentStateText(const QString& current_state) {
    const auto state = current_state.trimmed().toUpper();
    if (state == QStringLiteral("IDLE"))
        return QStringLiteral("대기");
    if (state == QStringLiteral("READY"))
        return QStringLiteral("준비 완료");
    if (state == QStringLiteral("RUNNING"))
        return QStringLiteral("가동 중");
    if (state == QStringLiteral("WORK_ASSIGNED"))
        return QStringLiteral("작업 할당됨");
    if (state == QStringLiteral("AWAITING_WORK_ID"))
        return QStringLiteral("작업 ID 대기");
    if (state == QStringLiteral("VISION_REPORTED"))
        return QStringLiteral("인식 완료");
    if (state == QStringLiteral("VISION_PROCESSING"))
        return QStringLiteral("영상 처리 중");
    if (state == QStringLiteral("PICKING"))
        return QStringLiteral("상품 집는 중");
    if (state == QStringLiteral("TRANSFERRING"))
        return QStringLiteral("컨베이어 사이 이송 중");
    if (state == QStringLiteral("PLACING"))
        return QStringLiteral("상품 내려놓는 중");
    if (state == QStringLiteral("SORTING"))
        return QStringLiteral("분류 중");
    if (state == QStringLiteral("DELIVERING"))
        return QStringLiteral("배송 중");
    if (state == QStringLiteral("STOPPED"))
        return QStringLiteral("정지");
    if (state == QStringLiteral("RECOVERY"))
        return QStringLiteral("복구 중");
    if (state == QStringLiteral("DISCONNECTED"))
        return QStringLiteral("연결 끊김");
    if (state == QStringLiteral("CAMERA_ERROR"))
        return QStringLiteral("카메라 오류");
    if (state == QStringLiteral("VISION_ERROR"))
        return QStringLiteral("비전 오류");
    if (state == QStringLiteral("ERROR"))
        return QStringLiteral("오류");
    return current_state;
}

}  // namespace

OperationsDashboardPanel::OperationsDashboardPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("operationsDashboard"));
    setMinimumHeight(112);
    setMaximumHeight(112);
    setStyleSheet(
        "#operationsDashboard{background:#1f1f1f;border-bottom:1px solid #2b2b2b;}"
        "#overallProcessCard,#processUnitCard{background:#181818;border:1px solid #2b2b2b;border-radius:6px;}"
        "#processStatusSection,#processStatusContent{background:#1f1f1f;border:0;}"
        "QLabel{color:#cccccc;}");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(8);

    auto* overall_card = new QFrame(this);
    overall_card->setObjectName(QStringLiteral("overallProcessCard"));
    overall_card->setFixedWidth(250);
    auto* overall_layout = new QVBoxLayout(overall_card);
    overall_layout->setContentsMargins(9, 6, 9, 6);
    overall_layout->setSpacing(2);
    auto* overall_header = new QHBoxLayout();
    overall_header->setContentsMargins(0, 0, 0, 0);
    auto* overall_title = new QLabel(QStringLiteral("전체 공정"), overall_card);
    overall_title->setStyleSheet("color:#f0f0f0;font-size:12px;font-weight:700;");
    overall_status_ = new QLabel(overall_card);
    overall_header->addWidget(overall_title);
    overall_header->addStretch();
    overall_header->addWidget(overall_status_);
    overall_summary_ = new QLabel(overall_card);
    overall_summary_->setStyleSheet("color:#75beff;font-size:11px;font-weight:700;");
    overall_work_count_ = new QLabel(overall_card);
    overall_work_count_->setStyleSheet("color:#cccccc;font-size:9px;");
    overall_detail_ = new QLabel(overall_card);
    overall_detail_->setStyleSheet("color:#f14c4c;font-size:9px;");
    overall_updated_at_ = new QLabel(overall_card);
    overall_updated_at_->setStyleSheet("color:#7f7f7f;font-size:9px;");
    overall_layout->addLayout(overall_header);
    overall_layout->addWidget(overall_summary_);
    overall_layout->addWidget(overall_work_count_);
    overall_layout->addWidget(overall_detail_);
    overall_layout->addWidget(overall_updated_at_);
    layout->addWidget(overall_card);

    auto* process_section = new QWidget(this);
    process_section->setObjectName(QStringLiteral("processStatusSection"));
    process_section->setAttribute(Qt::WA_StyledBackground);
    auto* process_section_layout = new QVBoxLayout(process_section);
    process_section_layout->setContentsMargins(0, 0, 0, 0);
    process_section_layout->setSpacing(3);
    auto* process_header = new QHBoxLayout();
    process_header->setContentsMargins(2, 0, 2, 0);
    auto* process_title = new QLabel(QStringLiteral("공정·지원 노드 실시간 상태"), process_section);
    process_title->setStyleSheet("color:#f0f0f0;font-size:10px;font-weight:700;");
    live_status_ = new QLabel(QStringLiteral("실시간 연결 대기"), process_section);
    live_status_->setStyleSheet("color:#9d9d9d;font-size:9px;font-weight:700;");
    process_header->addWidget(process_title);
    process_header->addStretch();
    process_header->addWidget(live_status_);
    process_section_layout->addLayout(process_header);

    auto* scroll_area = new QScrollArea(process_section);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);
    scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_area->setStyleSheet(
        "QScrollArea{background:#1f1f1f;border:0;}"
        "QScrollArea>QWidget>QWidget{background:#1f1f1f;}"
        "QScrollBar:horizontal{height:4px;background:#1f1f1f;}"
        "QScrollBar::handle:horizontal{background:#454545;border-radius:2px;min-width:24px;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}");
    scroll_area->viewport()->setStyleSheet("background:#1f1f1f;");
    auto* process_content = new QWidget(scroll_area);
    process_content->setObjectName(QStringLiteral("processStatusContent"));
    process_content->setAttribute(Qt::WA_StyledBackground);
    process_layout_ = new QHBoxLayout(process_content);
    process_layout_->setContentsMargins(0, 0, 0, 0);
    process_layout_->setSpacing(4);
    scroll_area->setWidget(process_content);
    process_section_layout->addWidget(scroll_area, 1);
    layout->addWidget(process_section, 1);

    timestamp_timer_ = new QTimer(this);
    timestamp_timer_->setInterval(1000);
    connect(timestamp_timer_, &QTimer::timeout, this, &OperationsDashboardPanel::refreshTimestamps);
    timestamp_timer_->start();
    refreshOverall();
}

void OperationsDashboardPanel::setState(const OperationsDashboardState& state) {
    overall_ = state.overall();
    const auto new_processes = state.processes();
    bool rebuild = new_processes.size() != processes_.size();
    if (!rebuild) {
        for (qsizetype index = 0; index < new_processes.size(); ++index) {
            if (new_processes[index].key != processes_[index].key) {
                rebuild = true;
                break;
            }
        }
    }
    processes_ = new_processes;
    if (rebuild) {
        rebuildProcessCards();
    }
    refreshOverall();
    refreshProcesses();
}

void OperationsDashboardPanel::setMqttConnected(bool connected) {
    live_status_->setText(connected ? QStringLiteral("● 실시간 수신 중") : QStringLiteral("● 실시간 수신 중단"));
    live_status_->setStyleSheet(connected ? QStringLiteral("color:#89d185;font-size:9px;font-weight:700;")
                                          : QStringLiteral("color:#f14c4c;font-size:9px;font-weight:700;"));
}

OperationsDashboardPanel::ProcessCardWidgets OperationsDashboardPanel::createProcessCard(
    const ProcessUnitStatus& process) {
    ProcessCardWidgets widgets;
    widgets.card = new QFrame(this);
    widgets.card->setObjectName(QStringLiteral("processUnitCard"));
    widgets.card->setAttribute(Qt::WA_StyledBackground);
    widgets.card->setMinimumWidth(170);
    widgets.card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    widgets.card->setToolTip(QStringLiteral("장치 ID: %1").arg(process.device_id));
    auto* layout = new QVBoxLayout(widgets.card);
    layout->setContentsMargins(8, 5, 8, 5);
    layout->setSpacing(1);
    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(process.display_name, widgets.card);
    title->setStyleSheet("color:#f0f0f0;font-size:10px;font-weight:700;");
    widgets.status = new QLabel(widgets.card);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(widgets.status);
    widgets.current_state = new QLabel(widgets.card);
    widgets.current_state->setStyleSheet("color:#75beff;font-size:10px;font-weight:600;");
    widgets.work_or_error = new QLabel(widgets.card);
    widgets.work_or_error->setStyleSheet("color:#9d9d9d;font-size:9px;");
    widgets.device_and_updated_at = new QLabel(widgets.card);
    widgets.device_and_updated_at->setStyleSheet("color:#7f7f7f;font-size:8px;");
    layout->addLayout(header);
    layout->addWidget(widgets.current_state);
    layout->addWidget(widgets.work_or_error);
    layout->addWidget(widgets.device_and_updated_at);
    return widgets;
}

void OperationsDashboardPanel::rebuildProcessCards() {
    process_cards_.clear();
    while (auto* item = process_layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto& process : processes_) {
        auto widgets = createProcessCard(process);
        process_layout_->addWidget(widgets.card, 1);
        process_cards_.insert(process.key, widgets);
    }
}

void OperationsDashboardPanel::refreshOverall() {
    const auto presentation = OverallPresentation(overall_.state);
    overall_status_->setText(presentation.text);
    overall_status_->setStyleSheet(PillStyle(presentation));
    overall_summary_->setText(overall_.stage);
    overall_work_count_->setText(
        QStringLiteral("가동 노드 %1 · 활성 작업 %2").arg(overall_.active_unit_count).arg(overall_.active_work_count));
    overall_detail_->setText(overall_.detail);
    overall_detail_->setVisible(!overall_.detail.isEmpty());
    overall_detail_->setToolTip(overall_.detail);
    overall_updated_at_->setText(UpdatedAtText(overall_.updated_at));
    overall_updated_at_->setToolTip(overall_.updated_at.toLocalTime().toString(Qt::ISODateWithMs));
}

void OperationsDashboardPanel::refreshProcesses() {
    for (const auto& process : processes_) {
        auto iterator = process_cards_.find(process.key);
        if (iterator == process_cards_.end()) {
            continue;
        }
        auto& widgets = iterator.value();
        const auto presentation = ProcessPresentation(process);
        widgets.status->setText(presentation.text);
        widgets.status->setStyleSheet(PillStyle(presentation));
        widgets.current_state->setText(CurrentStateText(process.current_state));
        widgets.current_state->setToolTip(process.current_state);
        if (process.has_error) {
            widgets.work_or_error->setText(process.error_code.isEmpty()
                                               ? QStringLiteral("오류 발생")
                                               : QStringLiteral("오류 · %1").arg(process.error_code));
            widgets.work_or_error->setStyleSheet("color:#f14c4c;font-size:9px;");
        } else {
            widgets.work_or_error->setText(process.work_id.isEmpty()
                                               ? QStringLiteral("작업 · 없음")
                                               : QStringLiteral("작업 · %1").arg(process.work_id));
            widgets.work_or_error->setStyleSheet("color:#9d9d9d;font-size:9px;");
        }
        widgets.work_or_error->setToolTip(process.work_id);
        widgets.device_and_updated_at->setText(
            QStringLiteral("%1 · %2").arg(process.device_id, UpdatedAtText(process.updated_at)));
        widgets.device_and_updated_at->setToolTip(process.updated_at.toLocalTime().toString(Qt::ISODateWithMs));
    }
}

void OperationsDashboardPanel::refreshTimestamps() {
    overall_updated_at_->setText(UpdatedAtText(overall_.updated_at));
    for (const auto& process : processes_) {
        auto iterator = process_cards_.find(process.key);
        if (iterator != process_cards_.end()) {
            iterator->device_and_updated_at->setText(
                QStringLiteral("%1 · %2").arg(process.device_id, UpdatedAtText(process.updated_at)));
        }
    }
}

}  // namespace logistics::control_center
