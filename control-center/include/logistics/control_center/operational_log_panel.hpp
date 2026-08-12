#pragma once

#include <QList>
#include <QWidget>
#include <functional>

#include "logistics/control_center/operational_log_state.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

namespace logistics::control_center {

class OperationalLogFilterProxyModel;
class OperationalLogTableModel;

class OperationalLogPanel final : public QWidget {
public:
    using AcknowledgeHandler = std::function<void(const QString& id)>;
    using AcknowledgeAllHandler = std::function<void()>;
    using EntryPageProvider = std::function<QList<OperationalLogEntry>(qsizetype offset, qsizetype limit)>;
    using OlderEntriesRequestHandler = std::function<void()>;

    explicit OperationalLogPanel(QWidget* parent = nullptr);

    void setEntryPageProvider(EntryPageProvider provider);
    void setMaximumEntries(qsizetype maximum_entries);
    void reloadEntries(int active_alert_count);
    void prependEntries(const QList<OperationalLogEntry>& entries, int active_alert_count);
    qsizetype appendOlderEntries(const QList<OperationalLogEntry>& entries, bool has_more, int active_alert_count);
    void setOlderEntriesRequestHandler(OlderEntriesRequestHandler handler);
    void setOlderEntriesLoading(bool loading);
    [[nodiscard]] bool canLoadOlderEntries() const;
    void requestOlderEntries();
    void setEntryAcknowledged(const QString& id, int active_alert_count);
    void setAllAlertsAcknowledged(int active_alert_count);
    void setAcknowledgeHandler(AcknowledgeHandler handler);
    void setAcknowledgeAllHandler(AcknowledgeAllHandler handler);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    [[nodiscard]] OperationalLogFilter currentFilter() const;
    [[nodiscard]] QString entryIdAtRow(int row) const;
    void applyFilter();
    void updateSummary();
    void acknowledgeEntry(const QString& id);
    void showDetails(const QString& id);
    void requestOlderEntriesAtBoundary();

    QLabel* alert_count_{ nullptr };
    QLabel* empty_state_{ nullptr };
    QLabel* result_count_{ nullptr };
    QComboBox* severity_filter_{ nullptr };
    QLineEdit* query_filter_{ nullptr };
    QCheckBox* unacknowledged_filter_{ nullptr };
    QPushButton* acknowledge_all_button_{ nullptr };
    QTableView* table_{ nullptr };
    OperationalLogTableModel* table_model_{ nullptr };
    OperationalLogFilterProxyModel* filter_model_{ nullptr };
    int active_alert_count_{ 0 };
    int pending_new_entry_count_{ 0 };
    AcknowledgeHandler acknowledge_handler_;
    AcknowledgeAllHandler acknowledge_all_handler_;
};

}  // namespace logistics::control_center
