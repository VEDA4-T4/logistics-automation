#include "logistics/control_center/operations_dashboard_panel.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "logistics/control_center/factory_top_view.hpp"

namespace logistics::control_center {
namespace {

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
    const auto state = BuildFactoryNodeVisual(process).state;
    const auto foreground = FactoryNodeColor(state).name();
    switch (state) {
        case FactoryNodeVisualState::Disconnected:
            return { QStringLiteral("연결 끊김"), QStringLiteral("#252526"), foreground, QStringLiteral("#3c3c3c") };
        case FactoryNodeVisualState::EmergencyStop:
            return { QStringLiteral("비상정지"), QStringLiteral("#521b20"), foreground, QStringLiteral("#a1262f") };
        case FactoryNodeVisualState::Error:
            return { QStringLiteral("오류"), QStringLiteral("#3b1f22"), foreground, QStringLiteral("#6e2b2f") };
        case FactoryNodeVisualState::Recovery:
            return { QStringLiteral("복구 중"), QStringLiteral("#17324a"), foreground, QStringLiteral("#285a7e") };
        case FactoryNodeVisualState::Stopped:
            return { QStringLiteral("정지"), QStringLiteral("#3a3000"), foreground, QStringLiteral("#6b5d00") };
        case FactoryNodeVisualState::Working:
            return { process.has_warning ? QStringLiteral("센서 경고") : QStringLiteral("작업 중"),
                     QStringLiteral("#17324a"), foreground, QStringLiteral("#285a7e") };
        case FactoryNodeVisualState::Running:
            return { process.has_warning ? QStringLiteral("센서 경고") : QStringLiteral("가동 중"),
                     QStringLiteral("#1f3325"), foreground, QStringLiteral("#385a40") };
        case FactoryNodeVisualState::Waiting:
            return { process.has_warning ? QStringLiteral("센서 경고") : QStringLiteral("대기"),
                     QStringLiteral("#252526"), foreground, QStringLiteral("#3c3c3c") };
    }
    return {};
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
    if (state == QStringLiteral("WAITING_FOR_PRODUCT"))
        return QStringLiteral("상품 감지 대기");
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
    if (state == QStringLiteral("RECOVERY_READY"))
        return QStringLiteral("복구 완료 · 시작 대기");
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

QString SensorStateText(const QString& measurement_status) {
    const auto status = measurement_status.trimmed().toUpper();
    if (status == QStringLiteral("CLEAR"))
        return QStringLiteral("없음");
    if (status == QStringLiteral("DETECTED"))
        return QStringLiteral("감지");
    if (status == QStringLiteral("FAULT"))
        return QStringLiteral("오류");
    return QStringLiteral("대기");
}

QString SensorStateColor(const QString& measurement_status) {
    const auto status = measurement_status.trimmed().toUpper();
    if (status == QStringLiteral("CLEAR"))
        return QStringLiteral("#89d185");
    if (status == QStringLiteral("DETECTED"))
        return QStringLiteral("#75beff");
    if (status == QStringLiteral("FAULT"))
        return QStringLiteral("#f14c4c");
    return QStringLiteral("#6e6e6e");
}

QString SensorIndicatorText(const SensorUnitStatus& sensor) {
    auto text = QStringLiteral("● %1 %2").arg(sensor.display_name, SensorStateText(sensor.measurement_status));
    if (sensor.distance_cm >= 0) {
        text.append(QStringLiteral(" · %1 cm").arg(sensor.distance_cm));
    }
    return text;
}

void SetElidedText(QLabel* label, const QString& text) {
    label->setText(label->fontMetrics().elidedText(text, Qt::ElideRight, 140));
    label->setToolTip(text);
}

}  // namespace

OperationsDashboardPanel::OperationsDashboardPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("operationsDashboard"));
    setMinimumHeight(104);
    setMaximumHeight(132);
    setStyleSheet(
        "#operationsDashboard{background:#1f1f1f;border-bottom:1px solid #303030;}"
        "#overallProcessCard,#processUnitCard{background:#181818;border:1px solid #303030;border-radius:6px;}"
        "#overallProcessCard[selectedControlTarget=\"true\"],#processUnitCard[selectedControlTarget=\"true\"]{"
        "background:#172534;border:2px solid #4daafc;}"
        "#overallProcessCard:focus,#processUnitCard:focus{border:2px solid #d7ba7d;}"
        "#overallProcessCard:hover,#processUnitCard:hover{border-color:#75beff;}"
        "#processStatusSection,#processStatusContent{background:#1f1f1f;border:0;}"
        "QLabel{color:#cccccc;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(4);

    overall_card_ = new QFrame(this);
    overall_card_->setObjectName(QStringLiteral("overallProcessCard"));
    overall_card_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    overall_card_->setCursor(Qt::PointingHandCursor);
    overall_card_->setFocusPolicy(Qt::StrongFocus);
    overall_card_->setAccessibleName(QStringLiteral("전체 공정 제어 대상"));
    overall_card_->setAccessibleDescription(QStringLiteral("Enter 또는 Space 키로 전체 공정을 제어 대상으로 선택"));
    overall_card_->setProperty("controlTargetDeviceId", QStringLiteral("SYSTEM"));
    overall_card_->installEventFilter(this);
    auto* overall_layout = new QVBoxLayout(overall_card_);
    overall_layout->setContentsMargins(9, 6, 9, 6);
    overall_layout->setSpacing(2);
    auto* overall_header = new QHBoxLayout();
    overall_header->setContentsMargins(0, 0, 0, 0);
    auto* overall_title = new QLabel(QStringLiteral("전체 공정"), overall_card_);
    overall_title->setStyleSheet("color:#f0f0f0;font-size:12px;font-weight:700;");
    overall_status_ = new QLabel(overall_card_);
    overall_header->addWidget(overall_title);
    overall_header->addStretch();
    overall_header->addWidget(overall_status_);
    overall_summary_ = new QLabel(overall_card_);
    overall_summary_->setStyleSheet("color:#75beff;font-size:11px;font-weight:700;");
    overall_work_count_ = new QLabel(overall_card_);
    overall_work_count_->setStyleSheet("color:#cccccc;font-size:9px;");
    overall_detail_ = new QLabel(overall_card_);
    overall_detail_->setStyleSheet("color:#f14c4c;font-size:9px;");
    overall_updated_at_ = new QLabel(overall_card_);
    overall_updated_at_->setStyleSheet("color:#7f7f7f;font-size:9px;");
    overall_layout->addLayout(overall_header);
    overall_layout->addWidget(overall_summary_);
    overall_layout->addWidget(overall_work_count_);
    overall_layout->addWidget(overall_detail_);
    overall_layout->addWidget(overall_updated_at_);
    for (auto* label : overall_card_->findChildren<QLabel*>()) {
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    auto* process_section = new QWidget(this);
    process_section->setObjectName(QStringLiteral("processStatusSection"));
    process_section->setAttribute(Qt::WA_StyledBackground);
    auto* process_section_layout = new QVBoxLayout(process_section);
    process_section_layout->setContentsMargins(0, 0, 0, 0);
    process_section_layout->setSpacing(8);
    auto* process_header = new QHBoxLayout();
    process_header->setContentsMargins(2, 0, 2, 0);
    auto* process_title = new QLabel(QStringLiteral("공정·지원 노드 실시간 상태"), process_section);
    process_title->setStyleSheet("color:#f0f0f0;font-size:10px;font-weight:700;");
    live_status_ = new QLabel(QStringLiteral("실시간 연결 대기"), process_section);
    live_status_->setObjectName(QStringLiteral("dashboardLiveStatus"));
    live_status_->setStyleSheet("color:#9d9d9d;font-size:9px;font-weight:700;");
    process_header->addWidget(process_title);
    process_header->addStretch();
    process_header->addWidget(live_status_);
    process_section_layout->addLayout(process_header);

    auto* process_content = new QWidget(process_section);
    process_content->setObjectName(QStringLiteral("processCardGrid"));
    process_content->setAttribute(Qt::WA_StyledBackground);
    process_layout_ = new QGridLayout(process_content);
    process_layout_->setContentsMargins(0, 0, 0, 0);
    process_layout_->setSpacing(8);
    process_layout_->addWidget(overall_card_, 0, 0);
    process_section_layout->addWidget(process_content, 1);
    layout->addWidget(process_section);

    timestamp_timer_ = new QTimer(this);
    timestamp_timer_->setInterval(1000);
    connect(timestamp_timer_, &QTimer::timeout, this, &OperationsDashboardPanel::refreshTimestamps);
    timestamp_timer_->start();
    refreshOverall();
    refreshControlTargetSelection();
    QTimer::singleShot(0, this, [this] { updateCardLayout(true); });
}

void OperationsDashboardPanel::setState(const OperationsDashboardState& state) {
    overall_ = state.overall();
    const auto new_processes = state.processes();
    bool rebuild = new_processes.size() != processes_.size();
    if (!rebuild) {
        for (qsizetype index = 0; index < new_processes.size(); ++index) {
            const auto& new_process = new_processes[index];
            const auto& previous_process = processes_[index];
            if (new_process.key != previous_process.key ||
                new_process.sensors.size() != previous_process.sensors.size()) {
                rebuild = true;
                break;
            }
            for (qsizetype sensor_index = 0; sensor_index < new_process.sensors.size(); ++sensor_index) {
                if (new_process.sensors[sensor_index].sensor_id != previous_process.sensors[sensor_index].sensor_id) {
                    rebuild = true;
                    break;
                }
            }
            if (rebuild) {
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
    live_status_->setText(connected ? QStringLiteral("● 실시간 수신 중") : QStringLiteral("● MQTT 연결 끊김"));
    live_status_->setStyleSheet(connected ? QStringLiteral("color:#89d185;font-size:9px;font-weight:700;")
                                          : QStringLiteral("color:#9d9d9d;font-size:9px;font-weight:700;"));
}

void OperationsDashboardPanel::setControlTarget(const QString& target_device_id) {
    selected_control_target_ = target_device_id.isEmpty() ? QStringLiteral("SYSTEM") : target_device_id;
    refreshControlTargetSelection();
}

bool OperationsDashboardPanel::eventFilter(QObject* watched, QEvent* event) {
    const bool mouse_activated =
        event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton;
    const bool keyboard_activated =
        event->type() == QEvent::KeyPress && (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Return ||
                                              static_cast<QKeyEvent*>(event)->key() == Qt::Key_Enter ||
                                              static_cast<QKeyEvent*>(event)->key() == Qt::Key_Space);
    if (mouse_activated || keyboard_activated) {
        const auto target_device_id = watched->property("controlTargetDeviceId").toString();
        if (!target_device_id.isEmpty()) {
            QString display_name = QStringLiteral("전체 공정");
            for (const auto& process : processes_) {
                if (process.device_id == target_device_id) {
                    display_name = process.display_name;
                    break;
                }
            }
            setControlTarget(target_device_id);
            emit controlTargetSelected(target_device_id, display_name);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OperationsDashboardPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateCardLayout();
}

OperationsDashboardPanel::ProcessCardWidgets OperationsDashboardPanel::createProcessCard(
    const ProcessUnitStatus& process) {
    ProcessCardWidgets widgets;
    widgets.card = new QFrame(this);
    widgets.card->setObjectName(QStringLiteral("processUnitCard"));
    widgets.card->setAttribute(Qt::WA_StyledBackground);
    widgets.card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    widgets.card->setCursor(Qt::PointingHandCursor);
    widgets.card->setFocusPolicy(Qt::StrongFocus);
    widgets.card->setAccessibleName(QStringLiteral("%1 제어 대상").arg(process.display_name));
    widgets.card->setAccessibleDescription(
        QStringLiteral("Enter 또는 Space 키로 %1을 제어 대상으로 선택").arg(process.display_name));
    widgets.card->setProperty("controlTargetDeviceId", process.device_id);
    widgets.card->installEventFilter(this);
    widgets.card->setToolTip(QStringLiteral("클릭하여 제어 대상으로 선택 · %1").arg(process.device_id));
    auto* layout = new QVBoxLayout(widgets.card);
    layout->setContentsMargins(8, 5, 8, 5);
    layout->setSpacing(1);
    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(process.display_name, widgets.card);
    title->setStyleSheet("color:#f0f0f0;font-size:10px;font-weight:700;");
    widgets.status = new QLabel(widgets.card);
    widgets.status->setObjectName(QStringLiteral("processVisualStatus"));
    header->addWidget(title);
    header->addStretch();
    header->addWidget(widgets.status);
    widgets.current_state = new QLabel(widgets.card);
    widgets.current_state->setStyleSheet("color:#75beff;font-size:10px;font-weight:600;");
    widgets.work_or_error = new QLabel(widgets.card);
    widgets.work_or_error->setStyleSheet("color:#9d9d9d;font-size:9px;");
    widgets.work_or_error->setMinimumWidth(0);
    widgets.work_or_error->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    widgets.device_and_updated_at = new QLabel(widgets.card);
    widgets.device_and_updated_at->setStyleSheet("color:#7f7f7f;font-size:8px;");
    widgets.device_and_updated_at->setMinimumWidth(0);
    widgets.device_and_updated_at->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* sensor_layout = new QHBoxLayout();
    sensor_layout->setContentsMargins(0, 0, 0, 0);
    sensor_layout->setSpacing(8);
    for (const auto& sensor : process.sensors) {
        auto* indicator = new QLabel(widgets.card);
        indicator->setObjectName(QStringLiteral("sensorStatusIndicator"));
        indicator->setProperty("sensorId", sensor.sensor_id);
        indicator->setProperty("measurementStatus", sensor.measurement_status);
        indicator->setProperty("distanceCm", sensor.distance_cm);
        indicator->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        sensor_layout->addWidget(indicator);
        widgets.sensor_indicators.insert(sensor.sensor_id, indicator);
    }
    sensor_layout->addStretch();
    layout->addLayout(header);
    layout->addWidget(widgets.current_state);
    layout->addWidget(widgets.work_or_error);
    if (!process.sensors.isEmpty()) {
        layout->addLayout(sensor_layout);
    }
    layout->addWidget(widgets.device_and_updated_at);
    for (auto* label : widgets.card->findChildren<QLabel*>()) {
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    return widgets;
}

void OperationsDashboardPanel::rebuildProcessCards() {
    for (const auto& widgets : process_cards_) {
        delete widgets.card;
    }
    process_cards_.clear();
    for (qsizetype index = 0; index < processes_.size(); ++index) {
        const auto& process = processes_[index];
        auto widgets = createProcessCard(process);
        process_cards_.insert(process.key, widgets);
    }
    updateCardLayout(true);
    refreshControlTargetSelection();
}

void OperationsDashboardPanel::updateCardLayout(bool force) {
    constexpr int kWideLayoutBreakpoint = 1400;
    const int columns = width() >= kWideLayoutBreakpoint ? 6 : 3;
    if (!force && card_column_count_ == columns) {
        return;
    }
    card_column_count_ = columns;

    QList<QWidget*> cards{ overall_card_ };
    for (const auto& process : processes_) {
        const auto card = process_cards_.constFind(process.key);
        if (card != process_cards_.cend()) {
            cards.append(card->card);
        }
    }
    for (auto* card : cards) {
        process_layout_->removeWidget(card);
    }
    for (qsizetype index = 0; index < cards.size(); ++index) {
        process_layout_->addWidget(cards[index], static_cast<int>(index) / columns, static_cast<int>(index) % columns);
    }
    for (int column = 0; column < 6; ++column) {
        process_layout_->setColumnStretch(column, column < columns ? 1 : 0);
    }

    const bool two_rows = cards.size() > columns;
    overall_updated_at_->setVisible(!two_rows);
    for (auto iterator = process_cards_.begin(); iterator != process_cards_.end(); ++iterator) {
        iterator->device_and_updated_at->setVisible(!two_rows);
    }
    setMinimumHeight(two_rows ? 148 : 104);
    setMaximumHeight(two_rows ? 168 : 132);
    updateGeometry();
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
        QString work_or_error;
        if (process.has_error) {
            work_or_error = process.error_code.isEmpty() ? QStringLiteral("오류 발생")
                                                         : QStringLiteral("오류 · %1").arg(process.error_code);
            widgets.work_or_error->setStyleSheet("color:#f14c4c;font-size:9px;");
        } else if (process.has_warning) {
            work_or_error = IsSensorStaleErrorCode(process.error_code)
                                ? QStringLiteral("경고 · 센서 응답 지연")
                                : QStringLiteral("경고 · %1").arg(process.error_code);
            widgets.work_or_error->setStyleSheet("color:#cca700;font-size:9px;");
        } else {
            work_or_error = process.work_id.isEmpty() ? QStringLiteral("작업 · 없음")
                                                      : QStringLiteral("작업 · %1").arg(process.work_id);
            widgets.work_or_error->setStyleSheet("color:#9d9d9d;font-size:9px;");
        }
        SetElidedText(widgets.work_or_error, work_or_error);
        const auto device_and_updated_at =
            QStringLiteral("%1 · %2").arg(process.device_id, UpdatedAtText(process.updated_at));
        SetElidedText(widgets.device_and_updated_at, device_and_updated_at);
        for (const auto& sensor : process.sensors) {
            auto* indicator = widgets.sensor_indicators.value(sensor.sensor_id, nullptr);
            if (indicator == nullptr) {
                continue;
            }
            indicator->setText(SensorIndicatorText(sensor));
            indicator->setStyleSheet(QStringLiteral("color:%1;font-size:8px;font-weight:700;")
                                         .arg(SensorStateColor(sensor.measurement_status)));
            indicator->setProperty("measurementStatus", sensor.measurement_status);
            indicator->setProperty("distanceCm", sensor.distance_cm);
            const auto distance =
                sensor.distance_cm >= 0 ? QStringLiteral("%1 cm").arg(sensor.distance_cm) : QStringLiteral("거리 없음");
            const auto updated_at =
                sensor.updated_at.isValid() ? UpdatedAtText(sensor.updated_at) : QStringLiteral("수신 기록 없음");
            indicator->setToolTip(
                QStringLiteral("%1 · %2 · %3 · %4")
                    .arg(sensor.display_name, SensorStateText(sensor.measurement_status), distance, updated_at));
        }
    }
}

void OperationsDashboardPanel::refreshTimestamps() {
    overall_updated_at_->setText(UpdatedAtText(overall_.updated_at));
    for (const auto& process : processes_) {
        auto iterator = process_cards_.find(process.key);
        if (iterator != process_cards_.end()) {
            SetElidedText(iterator->device_and_updated_at,
                          QStringLiteral("%1 · %2").arg(process.device_id, UpdatedAtText(process.updated_at)));
        }
    }
}

void OperationsDashboardPanel::refreshControlTargetSelection() {
    const auto update_card = [this](QFrame* card) {
        if (card == nullptr) {
            return;
        }
        card->setProperty("selectedControlTarget",
                          card->property("controlTargetDeviceId").toString() == selected_control_target_);
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    };
    update_card(overall_card_);
    for (const auto& widgets : process_cards_) {
        update_card(widgets.card);
    }
}

}  // namespace logistics::control_center
