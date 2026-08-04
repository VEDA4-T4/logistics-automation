#include "logistics/control_center/operational_log_panel.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace logistics::control_center {
namespace {

constexpr qsizetype kMaximumVisibleEntries = 200;

QString SeverityColor(OperationalLogSeverity severity) {
    switch (severity) {
        case OperationalLogSeverity::Info:
            return QStringLiteral("#75beff");
        case OperationalLogSeverity::Warning:
            return QStringLiteral("#cca700");
        case OperationalLogSeverity::Error:
            return QStringLiteral("#f14c4c");
        case OperationalLogSeverity::Critical:
            return QStringLiteral("#ff7b72");
    }
    return QStringLiteral("#cccccc");
}

QTableWidgetItem* ReadOnlyItem(const QString& text) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

}  // namespace

OperationalLogPanel::OperationalLogPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("operationalLogPanel"));
    setStyleSheet(
        "#operationalLogPanel{background:#181818;}"
        "QLineEdit{background:#202020;color:#d4d4d4;border:1px solid #303030;border-radius:6px;padding:5px;}"
        "QLineEdit:hover{border-color:#454545;}"
        "QCheckBox{color:#a8a8a8;spacing:6px;}"
        "QPushButton#acknowledgeAllLogsButton{color:#ff938a;border-color:#5d3235;background:#2a2021;}"
        "QTableWidget{background:#181818;color:#cccccc;border:0;gridline-color:transparent;outline:0;}"
        "QHeaderView::section{background:#181818;color:#777777;border:0;border-bottom:1px solid #303030;"
        "font-size:10px;font-weight:600;padding:4px 6px;}"
        "QTableWidget::item{border-bottom:1px solid #303030;padding:4px 6px;}"
        "QTableWidget::item:hover{background:#202a33;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout();
    auto* header_text = new QVBoxLayout();
    header_text->setContentsMargins(0, 0, 0, 0);
    header_text->setSpacing(1);
    auto* eyebrow = new QLabel(QStringLiteral("OPERATIONS"), this);
    eyebrow->setStyleSheet("color:#4daafc;font-size:8px;font-weight:700;letter-spacing:1px;");
    auto* title = new QLabel(QStringLiteral("운영 로그"), this);
    title->setStyleSheet("color:#f0f0f0;font-size:16px;font-weight:700;");
    header_text->addWidget(eyebrow);
    header_text->addWidget(title);
    alert_count_ = new QLabel(QStringLiteral("미확인 오류 0"), this);
    alert_count_->setObjectName(QStringLiteral("unacknowledgedAlertCount"));
    alert_count_->setStyleSheet(
        "background:#2c2021;color:#ff7b72;border:1px solid #5d3235;border-radius:9px;font-size:9px;"
        "font-weight:700;padding:3px 7px;");
    header->addLayout(header_text);
    header->addStretch();
    header->addWidget(alert_count_);
    layout->addLayout(header);

    auto* primary_filters = new QHBoxLayout();
    severity_filter_ = new QComboBox(this);
    severity_filter_->setObjectName(QStringLiteral("logSeverityFilter"));
    severity_filter_->addItem(QStringLiteral("전체 등급"), -1);
    severity_filter_->addItem(QStringLiteral("정보"), static_cast<int>(OperationalLogSeverity::Info));
    severity_filter_->addItem(QStringLiteral("경고"), static_cast<int>(OperationalLogSeverity::Warning));
    severity_filter_->addItem(QStringLiteral("오류"), static_cast<int>(OperationalLogSeverity::Error));
    severity_filter_->addItem(QStringLiteral("심각"), static_cast<int>(OperationalLogSeverity::Critical));
    query_filter_ = new QLineEdit(this);
    query_filter_->setObjectName(QStringLiteral("logQueryFilter"));
    query_filter_->setPlaceholderText(QStringLiteral("장치·코드·내용 검색"));
    query_filter_->setClearButtonEnabled(true);
    primary_filters->addWidget(severity_filter_, 0);
    primary_filters->addWidget(query_filter_, 1);
    layout->addLayout(primary_filters);

    auto* secondary_filters = new QHBoxLayout();
    unacknowledged_filter_ = new QCheckBox(QStringLiteral("미확인만"), this);
    unacknowledged_filter_->setObjectName(QStringLiteral("logUnacknowledgedOnly"));
    result_count_ = new QLabel(this);
    result_count_->setObjectName(QStringLiteral("operationalLogResultCount"));
    result_count_->setStyleSheet("color:#9d9d9d;font-size:10px;");
    auto* interaction_hint = new QLabel(QStringLiteral("클릭: 확인  ·  더블클릭: 상세 보기"), this);
    interaction_hint->setObjectName(QStringLiteral("operationalLogInteractionHint"));
    interaction_hint->setStyleSheet("color:#737373;font-size:9px;");
    acknowledge_all_button_ = new QPushButton(QStringLiteral("오류 전체 확인"), this);
    acknowledge_all_button_->setObjectName(QStringLiteral("acknowledgeAllLogsButton"));
    secondary_filters->addWidget(unacknowledged_filter_);
    secondary_filters->addWidget(result_count_);
    secondary_filters->addStretch();
    secondary_filters->addWidget(interaction_hint);
    secondary_filters->addWidget(acknowledge_all_button_);
    layout->addLayout(secondary_filters);

    auto* table_surface = new QWidget(this);
    auto* table_stack = new QStackedLayout(table_surface);
    table_stack->setContentsMargins(0, 0, 0, 0);
    table_stack->setStackingMode(QStackedLayout::StackAll);
    table_ = new QTableWidget(table_surface);
    table_->setObjectName(QStringLiteral("operationalLogTable"));
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels(
        { QStringLiteral("시각"), QStringLiteral("등급"), QStringLiteral("장치"), QStringLiteral("내용") });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(false);
    table_->setWordWrap(false);
    table_->viewport()->setCursor(Qt::PointingHandCursor);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(34);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_stack->addWidget(table_);
    auto* empty_state = new QLabel(QStringLiteral("표시할 운영 로그가 없습니다"), table_surface);
    empty_state->setObjectName(QStringLiteral("operationalLogEmptyState"));
    empty_state->setAlignment(Qt::AlignCenter);
    empty_state->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty_state->setStyleSheet("color:#777777;font-size:11px;");
    table_stack->addWidget(empty_state);
    layout->addWidget(table_surface, 1);

    connect(severity_filter_, &QComboBox::currentIndexChanged, this, [this]() { refresh(); });
    connect(query_filter_, &QLineEdit::textChanged, this, [this]() { refresh(); });
    connect(unacknowledged_filter_, &QCheckBox::toggled, this, [this]() { refresh(); });
    connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) { acknowledgeEntry(entryIdAtRow(row)); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const auto id = entryIdAtRow(row);
        acknowledgeEntry(id);
        showDetails(id);
    });
    connect(acknowledge_all_button_, &QPushButton::clicked, this, [this]() {
        if (acknowledge_all_handler_) {
            acknowledge_all_handler_();
        }
    });
    refresh();
}

void OperationalLogPanel::setState(const OperationalLogState& state) {
    state_ = state;
    refresh();
}

void OperationalLogPanel::setEntryAcknowledged(const QString& id) {
    if (!state_.acknowledge(id)) {
        return;
    }
    if (unacknowledged_filter_->isChecked()) {
        refresh();
        return;
    }

    const QSignalBlocker blocker(table_);
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (entryIdAtRow(row) != id) {
            continue;
        }
        for (int column = 0; column < table_->columnCount(); ++column) {
            if (auto* item = table_->item(row, column); item != nullptr) {
                item->setForeground(QColor(QStringLiteral("#777777")));
                item->setData(Qt::UserRole + 1, true);
            }
        }
        break;
    }
    alert_count_->setText(QStringLiteral("미확인 오류 %1").arg(state_.activeAlertCount()));
    alert_count_->setVisible(state_.activeAlertCount() > 0);
    acknowledge_all_button_->setEnabled(state_.activeAlertCount() > 0);
}

void OperationalLogPanel::setAcknowledgeHandler(AcknowledgeHandler handler) {
    acknowledge_handler_ = std::move(handler);
}

void OperationalLogPanel::setAcknowledgeAllHandler(AcknowledgeAllHandler handler) {
    acknowledge_all_handler_ = std::move(handler);
}

OperationalLogFilter OperationalLogPanel::currentFilter() const {
    OperationalLogFilter filter;
    const auto severity_value = severity_filter_->currentData().toInt();
    filter.filter_by_severity = severity_value >= 0;
    if (filter.filter_by_severity) {
        filter.severity = static_cast<OperationalLogSeverity>(severity_value);
    }
    filter.query = query_filter_->text();
    filter.unacknowledged_only = unacknowledged_filter_->isChecked();
    return filter;
}

void OperationalLogPanel::refresh() {
    const auto entries = state_.filteredEntries(currentFilter());
    const auto visible_count = std::min(entries.size(), kMaximumVisibleEntries);
    const QSignalBlocker blocker(table_);
    table_->setUpdatesEnabled(false);
    table_->setSortingEnabled(false);
    table_->setRowCount(visible_count);
    for (qsizetype row = 0; row < visible_count; ++row) {
        const auto& entry = entries[row];
        auto* time_item = ReadOnlyItem(entry.occurred_at.toLocalTime().toString(QStringLiteral("HH:mm:ss")));
        time_item->setData(Qt::UserRole, entry.id);
        time_item->setData(Qt::UserRole + 1, entry.acknowledged);
        time_item->setToolTip(entry.occurred_at.toLocalTime().toString(Qt::ISODateWithMs));
        auto* severity_item = ReadOnlyItem(OperationalSeverityLabel(entry.severity));
        severity_item->setForeground(QColor(SeverityColor(entry.severity)));
        QFont severity_font = severity_item->font();
        severity_font.setBold(true);
        severity_item->setFont(severity_font);
        auto* device_item = ReadOnlyItem(entry.device_id);
        const auto acknowledgement = entry.acknowledged ? QStringLiteral("확인됨") : QStringLiteral("미확인");
        const auto code_prefix = entry.code.isEmpty() ? QString{} : QStringLiteral("[%1] ").arg(entry.code);
        auto* message_item = ReadOnlyItem(QStringLiteral("%1%2 · %3").arg(code_prefix, entry.message, acknowledgement));
        const auto tooltip = QStringLiteral("분류: %1\n장치: %2\n코드: %3\n토픽: %4\n메시지 ID: %5")
                                 .arg(entry.category, entry.device_id, entry.code, entry.topic, entry.id);
        device_item->setToolTip(tooltip);
        message_item->setToolTip(tooltip);
        if (entry.acknowledged) {
            time_item->setForeground(QColor(QStringLiteral("#777777")));
            device_item->setForeground(QColor(QStringLiteral("#777777")));
            message_item->setForeground(QColor(QStringLiteral("#777777")));
        }
        table_->setItem(static_cast<int>(row), 0, time_item);
        table_->setItem(static_cast<int>(row), 1, severity_item);
        table_->setItem(static_cast<int>(row), 2, device_item);
        table_->setItem(static_cast<int>(row), 3, message_item);
    }
    table_->setUpdatesEnabled(true);
    result_count_->setText(entries.size() > visible_count
                               ? QStringLiteral("%1건 중 최근 %2건 표시").arg(entries.size()).arg(visible_count)
                               : QStringLiteral("%1건").arg(entries.size()));
    alert_count_->setText(QStringLiteral("미확인 오류 %1").arg(state_.activeAlertCount()));
    alert_count_->setVisible(state_.activeAlertCount() > 0);
    acknowledge_all_button_->setEnabled(state_.activeAlertCount() > 0);
    if (auto* empty_state = findChild<QLabel*>(QStringLiteral("operationalLogEmptyState")); empty_state != nullptr) {
        empty_state->setVisible(visible_count == 0);
    }
}

QString OperationalLogPanel::entryIdAtRow(int row) const {
    if (row < 0 || table_->item(row, 0) == nullptr) {
        return {};
    }
    return table_->item(row, 0)->data(Qt::UserRole).toString();
}

void OperationalLogPanel::acknowledgeEntry(const QString& id) {
    if (id.isEmpty() || !acknowledge_handler_) {
        return;
    }
    for (const auto& entry : state_.entries()) {
        if (entry.id == id) {
            if (!entry.acknowledged) {
                acknowledge_handler_(id);
            }
            return;
        }
    }
}

void OperationalLogPanel::showDetails(const QString& id) {
    const OperationalLogEntry* selected = nullptr;
    for (const auto& entry : state_.entries()) {
        if (entry.id == id) {
            selected = &entry;
            break;
        }
    }
    if (selected == nullptr) {
        return;
    }

    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("operationalLogDetailDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("운영 로그 상세"));
    dialog->resize(620, 430);
    dialog->setMinimumSize(520, 360);
    auto* dialog_layout = new QVBoxLayout(dialog);
    dialog_layout->setContentsMargins(20, 18, 20, 18);
    dialog_layout->setSpacing(13);

    auto* dialog_header = new QHBoxLayout();
    auto* severity = new QLabel(OperationalSeverityLabel(selected->severity), dialog);
    severity->setStyleSheet(QStringLiteral("background:%1;color:#181818;border-radius:9px;font-size:10px;"
                                           "font-weight:800;padding:4px 9px;")
                                .arg(SeverityColor(selected->severity)));
    auto* acknowledgement =
        new QLabel(selected->acknowledged ? QStringLiteral("확인됨") : QStringLiteral("미확인"), dialog);
    acknowledgement->setStyleSheet(selected->acknowledged
                                       ? QStringLiteral("color:#89d185;font-size:10px;font-weight:700;")
                                       : QStringLiteral("color:#cca700;font-size:10px;font-weight:700;"));
    dialog_header->addWidget(severity);
    dialog_header->addStretch();
    dialog_header->addWidget(acknowledgement);
    dialog_layout->addLayout(dialog_header);

    auto* detail_title = new QLabel(selected->code.isEmpty() ? selected->category : selected->code, dialog);
    detail_title->setObjectName(QStringLiteral("detailTitle"));
    detail_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dialog_layout->addWidget(detail_title);

    auto* metadata = new QFormLayout();
    metadata->setContentsMargins(0, 0, 0, 0);
    metadata->setHorizontalSpacing(18);
    metadata->setVerticalSpacing(7);
    const auto add_metadata = [dialog, metadata](const QString& label, const QString& value) {
        auto* key = new QLabel(label, dialog);
        key->setObjectName(QStringLiteral("detailMetaLabel"));
        auto* content = new QLabel(value.isEmpty() ? QStringLiteral("없음") : value, dialog);
        content->setObjectName(QStringLiteral("detailMetaValue"));
        content->setTextInteractionFlags(Qt::TextSelectableByMouse);
        content->setWordWrap(true);
        metadata->addRow(key, content);
    };
    add_metadata(QStringLiteral("발생 시각"), selected->occurred_at.toLocalTime().toString(Qt::ISODateWithMs));
    add_metadata(QStringLiteral("장치"), selected->device_id);
    add_metadata(QStringLiteral("분류"), selected->category);
    add_metadata(QStringLiteral("토픽"), selected->topic);
    add_metadata(QStringLiteral("메시지 ID"), selected->id);
    dialog_layout->addLayout(metadata);

    auto* message = new QPlainTextEdit(selected->message, dialog);
    message->setObjectName(QStringLiteral("operationalLogDetailMessage"));
    message->setReadOnly(true);
    message->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    dialog_layout->addWidget(message, 1);

    auto* actions = new QHBoxLayout();
    auto* close_button = new QPushButton(QStringLiteral("닫기"), dialog);
    close_button->setObjectName(QStringLiteral("closeOperationalLogDetail"));
    actions->addStretch();
    actions->addWidget(close_button);
    dialog_layout->addLayout(actions);
    connect(close_button, &QPushButton::clicked, dialog, &QDialog::close);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

}  // namespace logistics::control_center
