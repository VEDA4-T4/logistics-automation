#include "logistics/control_center/operational_log_panel.hpp"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <utility>

namespace logistics::control_center {
namespace {

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
        "QLabel{color:#cccccc;}"
        "QComboBox,QLineEdit{background:#252526;color:#cccccc;border:1px solid #3c3c3c;border-radius:4px;padding:5px;}"
        "QCheckBox{color:#cccccc;}"
        "QPushButton{background:#2d2d30;color:#f0f0f0;border:1px solid #454545;border-radius:4px;padding:5px 9px;}"
        "QPushButton:hover{background:#3a3a3d;}"
        "QPushButton:disabled{color:#777777;}"
        "QTableWidget{background:#181818;color:#cccccc;border:1px solid #2b2b2b;gridline-color:#2b2b2b;}"
        "QHeaderView::section{background:#252526;color:#cccccc;border:0;border-right:1px solid #3c3c3c;padding:5px;}"
        "QTableWidget::item:selected{background:#264f78;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(7);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("오류 알림 및 운영 로그"), this);
    title->setStyleSheet("color:#f0f0f0;font-size:14px;font-weight:700;");
    alert_count_ = new QLabel(QStringLiteral("미확인 오류 0"), this);
    alert_count_->setObjectName(QStringLiteral("unacknowledgedAlertCount"));
    alert_count_->setStyleSheet(
        "background:#3b1f22;color:#f14c4c;border:1px solid #6e2b2f;border-radius:4px;font-weight:700;padding:3px 6px;");
    header->addWidget(title);
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
    acknowledge_button_ = new QPushButton(QStringLiteral("선택 확인"), this);
    acknowledge_button_->setObjectName(QStringLiteral("acknowledgeLogButton"));
    secondary_filters->addWidget(unacknowledged_filter_);
    secondary_filters->addWidget(result_count_);
    secondary_filters->addStretch();
    secondary_filters->addWidget(acknowledge_button_);
    layout->addLayout(secondary_filters);

    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("operationalLogTable"));
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels(
        { QStringLiteral("시각"), QStringLiteral("등급"), QStringLiteral("장치"), QStringLiteral("내용") });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(34);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(table_, 1);

    connect(severity_filter_, &QComboBox::currentIndexChanged, this, [this]() { refresh(); });
    connect(query_filter_, &QLineEdit::textChanged, this, [this]() { refresh(); });
    connect(unacknowledged_filter_, &QCheckBox::toggled, this, [this]() { refresh(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const auto row = table_->currentRow();
        const bool can_acknowledge =
            row >= 0 && table_->item(row, 0) != nullptr && !table_->item(row, 0)->data(Qt::UserRole + 1).toBool();
        acknowledge_button_->setEnabled(can_acknowledge);
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { acknowledgeSelected(); });
    connect(acknowledge_button_, &QPushButton::clicked, this, &OperationalLogPanel::acknowledgeSelected);
    refresh();
}

void OperationalLogPanel::setState(const OperationalLogState& state) {
    state_ = state;
    refresh();
}

void OperationalLogPanel::setAcknowledgeHandler(AcknowledgeHandler handler) {
    acknowledge_handler_ = std::move(handler);
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
    table_->setSortingEnabled(false);
    table_->setRowCount(entries.size());
    for (qsizetype row = 0; row < entries.size(); ++row) {
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
    result_count_->setText(QStringLiteral("%1건").arg(entries.size()));
    alert_count_->setText(QStringLiteral("미확인 오류 %1").arg(state_.activeAlertCount()));
    alert_count_->setVisible(state_.activeAlertCount() > 0);
    acknowledge_button_->setEnabled(false);
}

void OperationalLogPanel::acknowledgeSelected() {
    const auto row = table_->currentRow();
    if (row < 0 || table_->item(row, 0) == nullptr || table_->item(row, 0)->data(Qt::UserRole + 1).toBool()) {
        return;
    }
    const auto id = table_->item(row, 0)->data(Qt::UserRole).toString();
    if (acknowledge_handler_ && !id.isEmpty()) {
        acknowledge_handler_(id);
    }
}

}  // namespace logistics::control_center
