#include "logistics/control_center/operational_log_panel.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
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
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <utility>

namespace logistics::control_center {
namespace {

constexpr qsizetype kPageSize = OperationalLogState::kPageSize;

enum OperationalLogDataRole {
    kEntryIdRole = Qt::UserRole + 1,
    kAcknowledgedRole,
    kSeverityRole,
    kSearchTextRole,
};

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

QString EntryTooltip(const OperationalLogEntry& entry) {
    return QStringLiteral("분류: %1\n장치: %2\n코드: %3\n토픽: %4\n메시지 ID: %5")
        .arg(entry.category, entry.device_id, entry.code, entry.topic, entry.id);
}

}  // namespace

class OperationalLogTableModel final : public QAbstractTableModel {
public:
    explicit OperationalLogTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setMaximumEntries(qsizetype maximum_entries) {
        maximum_entries_ = std::max<qsizetype>(kPageSize, maximum_entries);
        reload();
    }

    void setPageProvider(OperationalLogPanel::EntryPageProvider provider) {
        page_provider_ = std::move(provider);
        reload();
    }

    void setOlderEntriesRequestHandler(OperationalLogPanel::OlderEntriesRequestHandler handler) {
        older_entries_request_handler_ = std::move(handler);
        older_entries_available_ = static_cast<bool>(older_entries_request_handler_);
        has_more_ = has_more_ || older_entries_available_;
    }

    void setOlderEntriesLoading(bool loading) {
        older_entries_loading_ = loading;
    }

    void reload() {
        QList<OperationalLogEntry> first_page;
        loaded_capacity_ = std::min(kPageSize, maximum_entries_);
        if (page_provider_) {
            first_page = page_provider_(0, loaded_capacity_);
        }
        if (first_page.size() > loaded_capacity_) {
            first_page.resize(loaded_capacity_);
        }

        beginResetModel();
        entries_.clear();
        loaded_ids_.clear();
        older_entries_loading_ = false;
        next_offset_ = first_page.size();
        for (auto& entry : first_page) {
            if (!entry.id.isEmpty() && loaded_ids_.contains(entry.id)) {
                continue;
            }
            loaded_ids_.insert(entry.id);
            entries_.append(std::move(entry));
        }
        has_more_ = (page_provider_ && first_page.size() == kPageSize) || older_entries_available_;
        endResetModel();
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(entries_.size());
    }

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 4;
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size()) {
            return {};
        }
        const auto& entry = entries_[index.row()];
        if (role == kEntryIdRole)
            return entry.id;
        if (role == kAcknowledgedRole)
            return entry.acknowledged;
        if (role == kSeverityRole)
            return static_cast<int>(entry.severity);
        if (role == kSearchTextRole) {
            return QStringLiteral("%1 %2 %3 %4 %5 %6")
                .arg(entry.device_id, entry.category, entry.code, entry.message, entry.topic, entry.id);
        }
        if (role == Qt::ToolTipRole) {
            return index.column() == 0 ? entry.occurred_at.toLocalTime().toString(Qt::ISODateWithMs)
                                       : EntryTooltip(entry);
        }
        if (role == Qt::ForegroundRole) {
            if (entry.acknowledged && index.column() != 1) {
                return QColor(QStringLiteral("#777777"));
            }
            if (index.column() == 1) {
                return QColor(SeverityColor(entry.severity));
            }
            return {};
        }
        if (role == Qt::FontRole && index.column() == 1) {
            QFont font;
            font.setBold(true);
            return font;
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (index.column()) {
            case 0:
                return entry.occurred_at.toLocalTime().toString(QStringLiteral("HH:mm:ss"));
            case 1:
                return OperationalSeverityLabel(entry.severity);
            case 2:
                return entry.device_id;
            case 3: {
                const auto acknowledgement = entry.acknowledged ? QStringLiteral("확인됨") : QStringLiteral("미확인");
                const auto code_prefix = entry.code.isEmpty() ? QString{} : QStringLiteral("[%1] ").arg(entry.code);
                return QStringLiteral("%1%2 · %3").arg(code_prefix, entry.message, acknowledgement);
            }
            default:
                return {};
        }
    }

    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        static const QStringList headers = { QStringLiteral("시각"), QStringLiteral("등급"), QStringLiteral("장치"),
                                             QStringLiteral("내용") };
        return section >= 0 && section < headers.size() ? headers[section] : QVariant{};
    }

    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override {
        return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }

    [[nodiscard]] bool canLoadOlderEntries() const {
        const bool can_load_local_page = page_provider_ && !page_provider_(next_offset_, 1).isEmpty();
        const bool can_request_server_page = older_entries_available_ && older_entries_request_handler_;
        return has_more_ && !older_entries_loading_ && (can_load_local_page || can_request_server_page);
    }

    [[nodiscard]] bool canFetchMore(const QModelIndex&) const override {
        return false;
    }

    void fetchMore(const QModelIndex&) override {}

    void requestOlderEntries() {
        if (!canLoadOlderEntries()) {
            return;
        }
        if (older_entries_request_handler_ && older_entries_available_) {
            const auto local_page =
                page_provider_ ? page_provider_(next_offset_, kPageSize) : QList<OperationalLogEntry>{};
            if (local_page.isEmpty()) {
                older_entries_loading_ = true;
                older_entries_request_handler_();
                return;
            }
        }
        auto page = page_provider_ ? page_provider_(next_offset_, kPageSize) : QList<OperationalLogEntry>{};
        if (page.size() > kPageSize) {
            page.resize(kPageSize);
        }
        if (page.isEmpty()) {
            has_more_ = false;
            return;
        }
        loaded_capacity_ = std::min(maximum_entries_, loaded_capacity_ + kPageSize);

        QList<OperationalLogEntry> unique_entries;
        QSet<QString> new_ids;
        unique_entries.reserve(page.size());
        for (auto& entry : page) {
            if (!entry.id.isEmpty() && (loaded_ids_.contains(entry.id) || new_ids.contains(entry.id))) {
                continue;
            }
            new_ids.insert(entry.id);
            unique_entries.append(std::move(entry));
        }
        if (unique_entries.size() > maximum_entries_) {
            unique_entries.resize(maximum_entries_);
        }
        if (unique_entries.isEmpty()) {
            next_offset_ += page.size();
            has_more_ = older_entries_available_ || (page_provider_ && !page_provider_(next_offset_, 1).isEmpty());
            return;
        }
        const auto overflow = entries_.size() + unique_entries.size() - maximum_entries_;
        if (overflow > 0) {
            beginRemoveRows({}, 0, static_cast<int>(overflow - 1));
            for (qsizetype row = 0; row < overflow; ++row) {
                loaded_ids_.remove(entries_[row].id);
            }
            entries_.remove(0, overflow);
            endRemoveRows();
        }
        for (const auto& entry : unique_entries) {
            loaded_ids_.insert(entry.id);
        }
        const auto first_row = entries_.size();
        beginInsertRows({}, static_cast<int>(first_row), static_cast<int>(first_row + unique_entries.size() - 1));
        entries_.append(std::move(unique_entries));
        endInsertRows();
        next_offset_ += page.size();
        has_more_ = older_entries_available_ || (page_provider_ && !page_provider_(next_offset_, 1).isEmpty());
    }

    qsizetype appendOlderEntries(QList<OperationalLogEntry> entries, bool has_more) {
        older_entries_loading_ = false;
        QList<OperationalLogEntry> unique_entries;
        QSet<QString> new_ids;
        unique_entries.reserve(entries.size());
        for (auto& entry : entries) {
            if (!entry.id.isEmpty() && (loaded_ids_.contains(entry.id) || new_ids.contains(entry.id))) {
                continue;
            }
            new_ids.insert(entry.id);
            unique_entries.append(std::move(entry));
        }
        if (unique_entries.size() > maximum_entries_) {
            unique_entries.resize(maximum_entries_);
        }
        const auto required_capacity = std::min(maximum_entries_, entries_.size() + unique_entries.size());
        while (loaded_capacity_ < required_capacity && loaded_capacity_ < maximum_entries_) {
            loaded_capacity_ = std::min(maximum_entries_, loaded_capacity_ + kPageSize);
        }
        older_entries_available_ = has_more;
        if (unique_entries.isEmpty()) {
            has_more_ = older_entries_available_ || (page_provider_ && !page_provider_(next_offset_, 1).isEmpty());
            return 0;
        }
        const auto overflow = entries_.size() + unique_entries.size() - maximum_entries_;
        if (overflow > 0) {
            beginRemoveRows({}, 0, static_cast<int>(overflow - 1));
            for (qsizetype row = 0; row < overflow; ++row) {
                loaded_ids_.remove(entries_[row].id);
            }
            entries_.remove(0, overflow);
            endRemoveRows();
        }
        for (const auto& entry : unique_entries) {
            loaded_ids_.insert(entry.id);
        }
        const auto first_row = entries_.size();
        beginInsertRows({}, static_cast<int>(first_row), static_cast<int>(first_row + unique_entries.size() - 1));
        entries_.append(std::move(unique_entries));
        endInsertRows();
        has_more_ = older_entries_available_ || (page_provider_ && !page_provider_(next_offset_, 1).isEmpty());
        return entries_.size() - first_row;
    }

    qsizetype prependEntries(QList<OperationalLogEntry> entries) {
        QList<OperationalLogEntry> unique_entries;
        QSet<QString> new_ids;
        unique_entries.reserve(entries.size());
        for (auto& entry : entries) {
            if (!entry.id.isEmpty() && (loaded_ids_.contains(entry.id) || new_ids.contains(entry.id))) {
                continue;
            }
            new_ids.insert(entry.id);
            unique_entries.append(std::move(entry));
        }
        if (unique_entries.size() > loaded_capacity_) {
            unique_entries.resize(loaded_capacity_);
        }
        if (unique_entries.isEmpty()) {
            return 0;
        }
        for (const auto& entry : unique_entries) {
            loaded_ids_.insert(entry.id);
        }

        beginInsertRows({}, 0, static_cast<int>(unique_entries.size() - 1));
        for (auto index = unique_entries.size(); index > 0; --index) {
            entries_.prepend(std::move(unique_entries[index - 1]));
        }
        endInsertRows();
        const auto overflow = entries_.size() - loaded_capacity_;
        if (overflow > 0) {
            const auto first_row = loaded_capacity_;
            const auto last_row = entries_.size() - 1;
            beginRemoveRows({}, static_cast<int>(first_row), static_cast<int>(last_row));
            for (auto row = first_row; row <= last_row; ++row) {
                loaded_ids_.remove(entries_[row].id);
            }
            entries_.remove(first_row, overflow);
            endRemoveRows();
        }
        if (overflow > 0) {
            next_offset_ = entries_.size();
        } else {
            next_offset_ += unique_entries.size();
        }
        has_more_ = has_more_ || (page_provider_ && !page_provider_(next_offset_, 1).isEmpty());
        return unique_entries.size();
    }

    bool acknowledge(const QString& id) {
        for (qsizetype row = 0; row < entries_.size(); ++row) {
            auto& entry = entries_[row];
            if (entry.id != id || entry.acknowledged) {
                continue;
            }
            entry.acknowledged = true;
            emit dataChanged(index(static_cast<int>(row), 0), index(static_cast<int>(row), 3),
                             { Qt::DisplayRole, Qt::ForegroundRole, kAcknowledgedRole });
            return true;
        }
        return false;
    }

    void acknowledgeAllAlerts() {
        for (qsizetype row = 0; row < entries_.size(); ++row) {
            auto& entry = entries_[row];
            const bool is_alert =
                entry.severity == OperationalLogSeverity::Error || entry.severity == OperationalLogSeverity::Critical;
            if (!is_alert || entry.acknowledged) {
                continue;
            }
            entry.acknowledged = true;
            emit dataChanged(index(static_cast<int>(row), 0), index(static_cast<int>(row), 3),
                             { Qt::DisplayRole, Qt::ForegroundRole, kAcknowledgedRole });
        }
    }

    [[nodiscard]] const OperationalLogEntry* entryById(const QString& id) const {
        for (const auto& entry : entries_) {
            if (entry.id == id) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool hasMore() const noexcept {
        return has_more_;
    }
    [[nodiscard]] int activeAlertCount() const noexcept {
        return static_cast<int>(std::count_if(entries_.cbegin(), entries_.cend(), [](const auto& entry) {
            const bool is_alert =
                entry.severity == OperationalLogSeverity::Error || entry.severity == OperationalLogSeverity::Critical;
            return is_alert && !entry.acknowledged;
        }));
    }
    [[nodiscard]] bool isLoadingOlderEntries() const noexcept {
        return older_entries_loading_;
    }

private:
    OperationalLogPanel::EntryPageProvider page_provider_;
    OperationalLogPanel::OlderEntriesRequestHandler older_entries_request_handler_;
    QList<OperationalLogEntry> entries_;
    QSet<QString> loaded_ids_;
    qsizetype next_offset_{ 0 };
    qsizetype loaded_capacity_{ kPageSize };
    qsizetype maximum_entries_{ OperationalLogState::kDefaultMaximumEntries };
    bool has_more_{ false };
    bool older_entries_loading_{ false };
    bool older_entries_available_{ false };
};

class OperationalLogFilterProxyModel final : public QSortFilterProxyModel {
public:
    explicit OperationalLogFilterProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}

    void setOperationalFilter(OperationalLogFilter filter) {
        beginFilterChange();
        filter_ = std::move(filter);
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
    }

protected:
    [[nodiscard]] bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override {
        const auto source_index = sourceModel()->index(source_row, 0, source_parent);
        if (filter_.filter_by_severity &&
            source_index.data(kSeverityRole).toInt() != static_cast<int>(filter_.severity)) {
            return false;
        }
        if (filter_.unacknowledged_only && source_index.data(kAcknowledgedRole).toBool()) {
            return false;
        }
        return filter_.query.trimmed().isEmpty() ||
               source_index.data(kSearchTextRole).toString().contains(filter_.query.trimmed(), Qt::CaseInsensitive);
    }

private:
    OperationalLogFilter filter_;
};

OperationalLogPanel::OperationalLogPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("operationalLogPanel"));
    setStyleSheet(
        "#operationalLogPanel{background:#181818;}"
        "QLineEdit{background:#202020;color:#d4d4d4;border:1px solid #303030;border-radius:6px;padding:5px;}"
        "QLineEdit:hover{border-color:#454545;}"
        "QCheckBox{color:#a8a8a8;spacing:6px;}"
        "QPushButton#acknowledgeAllLogsButton{color:#ff938a;border-color:#5d3235;background:#2a2021;}"
        "QTableView{background:#181818;color:#cccccc;border:0;gridline-color:transparent;outline:0;}"
        "QHeaderView::section{background:#181818;color:#777777;border:0;border-bottom:1px solid #303030;"
        "font-size:10px;font-weight:600;padding:4px 6px;}"
        "QTableView::item{border-bottom:1px solid #303030;padding:4px 6px;}"
        "QTableView::item:hover{background:#202a33;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(4);

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
    table_surface->setObjectName(QStringLiteral("operationalLogTableSurface"));
    table_surface->setMinimumHeight(60);
    auto* table_layout = new QVBoxLayout(table_surface);
    table_layout->setContentsMargins(0, 0, 0, 0);
    table_model_ = new OperationalLogTableModel(this);
    filter_model_ = new OperationalLogFilterProxyModel(this);
    filter_model_->setSourceModel(table_model_);
    filter_model_->setDynamicSortFilter(true);
    table_ = new QTableView(table_surface);
    table_->setObjectName(QStringLiteral("operationalLogTable"));
    table_->setModel(filter_model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(false);
    table_->setWordWrap(false);
    table_->installEventFilter(this);
    table_->viewport()->setCursor(Qt::PointingHandCursor);
    table_->viewport()->installEventFilter(this);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(34);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_layout->addWidget(table_);
    empty_state_ = new QLabel(QStringLiteral("표시할 운영로그 없음"), table_->viewport());
    empty_state_->setObjectName(QStringLiteral("operationalLogEmptyState"));
    empty_state_->setAlignment(Qt::AlignCenter);
    empty_state_->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty_state_->setStyleSheet("color:#777777;font-size:11px;");
    empty_state_->setGeometry(table_->viewport()->rect());
    empty_state_->raise();
    layout->addWidget(table_surface, 1);

    connect(severity_filter_, &QComboBox::currentIndexChanged, this, [this]() { applyFilter(); });
    connect(query_filter_, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(unacknowledged_filter_, &QCheckBox::toggled, this, [this]() { applyFilter(); });
    connect(table_, &QTableView::clicked, this,
            [this](const QModelIndex& index) { acknowledgeEntry(entryIdAtRow(index.row())); });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto id = entryIdAtRow(index.row());
        acknowledgeEntry(id);
        showDetails(id);
    });
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        auto* scroll_bar = table_->verticalScrollBar();
        if (value == scroll_bar->minimum() && pending_new_entry_count_ > 0) {
            pending_new_entry_count_ = 0;
            updateSummary();
        }
    });
    connect(table_->verticalScrollBar(), &QScrollBar::sliderReleased, this, [this]() {
        auto* scroll_bar = table_->verticalScrollBar();
        if (scroll_bar->value() == scroll_bar->maximum()) {
            requestOlderEntriesAtBoundary();
        }
    });
    connect(acknowledge_all_button_, &QPushButton::clicked, this, [this]() {
        if (acknowledge_all_handler_) {
            acknowledge_all_handler_();
        }
    });
    applyFilter();
}

void OperationalLogPanel::setEntryPageProvider(EntryPageProvider provider) {
    table_model_->setPageProvider(std::move(provider));
    pending_new_entry_count_ = 0;
    updateSummary();
}

void OperationalLogPanel::setEntryCountProvider(EntryCountProvider provider) {
    entry_count_provider_ = std::move(provider);
    updateSummary();
}

void OperationalLogPanel::setMaximumEntries(qsizetype maximum_entries) {
    table_model_->setMaximumEntries(maximum_entries);
    pending_new_entry_count_ = 0;
    updateSummary();
}

void OperationalLogPanel::reloadEntries(int active_alert_count) {
    active_alert_count_ = active_alert_count;
    pending_new_entry_count_ = 0;
    table_model_->reload();
    table_->scrollToTop();
    updateSummary();
}

void OperationalLogPanel::prependEntries(const QList<OperationalLogEntry>& entries, int active_alert_count) {
    auto* scroll_bar = table_->verticalScrollBar();
    const bool follows_latest = scroll_bar->value() == scroll_bar->minimum();
    const int previous_scroll_value = scroll_bar->value();
    active_alert_count_ = active_alert_count;
    const auto inserted_count = table_model_->prependEntries(entries);
    if (inserted_count == 0) {
        updateSummary();
        return;
    }

    int visible_inserted_count = 0;
    for (qsizetype row = 0; row < inserted_count; ++row) {
        if (filter_model_->mapFromSource(table_model_->index(static_cast<int>(row), 0)).isValid()) {
            ++visible_inserted_count;
        }
    }
    if (follows_latest) {
        table_->scrollToTop();
    } else {
        pending_new_entry_count_ += static_cast<int>(inserted_count);
        scroll_bar->setValue(previous_scroll_value +
                             visible_inserted_count * table_->verticalHeader()->defaultSectionSize());
    }
    updateSummary();
}

qsizetype OperationalLogPanel::appendOlderEntries(const QList<OperationalLogEntry>& entries, bool has_more,
                                                  int active_alert_count) {
    active_alert_count_ = active_alert_count;
    const auto inserted_count = table_model_->appendOlderEntries(entries, has_more);
    updateSummary();
    return inserted_count;
}

void OperationalLogPanel::setOlderEntriesRequestHandler(OlderEntriesRequestHandler handler) {
    table_model_->setOlderEntriesRequestHandler(std::move(handler));
    updateSummary();
}

void OperationalLogPanel::setOlderEntriesLoading(bool loading) {
    table_model_->setOlderEntriesLoading(loading);
    updateSummary();
}

bool OperationalLogPanel::canLoadOlderEntries() const {
    return table_model_->canLoadOlderEntries();
}

void OperationalLogPanel::requestOlderEntries() {
    table_model_->requestOlderEntries();
    updateSummary();
}

void OperationalLogPanel::setEntryAcknowledged(const QString& id, int active_alert_count) {
    active_alert_count_ = active_alert_count;
    table_model_->acknowledge(id);
    updateSummary();
}

void OperationalLogPanel::setAllAlertsAcknowledged(int active_alert_count) {
    active_alert_count_ = active_alert_count;
    table_model_->acknowledgeAllAlerts();
    updateSummary();
}

void OperationalLogPanel::setAcknowledgeHandler(AcknowledgeHandler handler) {
    acknowledge_handler_ = std::move(handler);
}

void OperationalLogPanel::setAcknowledgeAllHandler(AcknowledgeAllHandler handler) {
    acknowledge_all_handler_ = std::move(handler);
}

bool OperationalLogPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_->viewport() && event->type() == QEvent::Resize && empty_state_ != nullptr) {
        empty_state_->setGeometry(table_->viewport()->rect());
    }
    if ((watched == table_ || watched == table_->viewport()) && event->type() == QEvent::Wheel) {
        auto* wheel_event = static_cast<QWheelEvent*>(event);
        auto vertical_delta =
            wheel_event->pixelDelta().y() != 0 ? wheel_event->pixelDelta().y() : wheel_event->angleDelta().y();
        if (wheel_event->inverted()) {
            vertical_delta = -vertical_delta;
        }
        if (vertical_delta != 0) {
            if (vertical_delta < 0) {
                QTimer::singleShot(0, this, [this]() { requestOlderEntriesAtBoundary(); });
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OperationalLogPanel::requestOlderEntriesAtBoundary() {
    if (table_model_->isLoadingOlderEntries()) {
        return;
    }
    auto* scroll_bar = table_->verticalScrollBar();
    if (scroll_bar->value() == scroll_bar->maximum() && table_model_->canLoadOlderEntries()) {
        table_model_->requestOlderEntries();
        updateSummary();
    }
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

void OperationalLogPanel::applyFilter() {
    filter_model_->setOperationalFilter(currentFilter());
    updateSummary();
}

void OperationalLogPanel::updateSummary() {
    const auto filtered_count = filter_model_->rowCount();
    const auto displayed_alert_count = std::max(active_alert_count_, table_model_->activeAlertCount());
    QString summary = QStringLiteral("%1건 표시").arg(filtered_count);
    if (entry_count_provider_) {
        const auto buffered_count = entry_count_provider_();
        if (buffered_count > filtered_count) {
            summary += QStringLiteral(" · 전체 %1건").arg(buffered_count);
        }
    }
    if (table_model_->isLoadingOlderEntries()) {
        summary += QStringLiteral(" · 이전 로그 불러오는 중…");
    } else if (table_model_->hasMore()) {
        summary += QStringLiteral(" · 아래로 스크롤해 이전 로그 보기");
    }
    if (pending_new_entry_count_ > 0) {
        summary += QStringLiteral(" · 새 로그 %1건").arg(pending_new_entry_count_);
    }
    result_count_->setText(summary);
    alert_count_->setText(QStringLiteral("미확인 오류 %1").arg(displayed_alert_count));
    alert_count_->setVisible(displayed_alert_count > 0);
    acknowledge_all_button_->setEnabled(displayed_alert_count > 0);
    const bool is_empty = filtered_count == 0;
    empty_state_->setVisible(is_empty);
    if (is_empty) {
        empty_state_->setGeometry(table_->viewport()->rect());
        empty_state_->raise();
    }
}

QString OperationalLogPanel::entryIdAtRow(int row) const {
    if (row < 0) {
        return {};
    }
    return filter_model_->index(row, 0).data(kEntryIdRole).toString();
}

void OperationalLogPanel::acknowledgeEntry(const QString& id) {
    if (id.isEmpty() || !acknowledge_handler_) {
        return;
    }
    const auto* entry = table_model_->entryById(id);
    if (entry != nullptr && !entry->acknowledged) {
        acknowledge_handler_(id);
    }
}

void OperationalLogPanel::showDetails(const QString& id) {
    const auto* selected = table_model_->entryById(id);
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
