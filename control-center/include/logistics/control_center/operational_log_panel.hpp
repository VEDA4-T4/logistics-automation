#pragma once

#include <QWidget>
#include <functional>

#include "logistics/control_center/operational_log_state.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace logistics::control_center {

class OperationalLogPanel final : public QWidget {
public:
    using AcknowledgeHandler = std::function<void(const QString& id)>;
    using AcknowledgeAllHandler = std::function<void()>;

    explicit OperationalLogPanel(QWidget* parent = nullptr);

    void setState(const OperationalLogState& state);
    void setAcknowledgeHandler(AcknowledgeHandler handler);
    void setAcknowledgeAllHandler(AcknowledgeAllHandler handler);

private:
    [[nodiscard]] OperationalLogFilter currentFilter() const;
    void refresh();
    void acknowledgeSelected();
    void showDetails(int row);

    QLabel* alert_count_{ nullptr };
    QLabel* result_count_{ nullptr };
    QComboBox* severity_filter_{ nullptr };
    QLineEdit* query_filter_{ nullptr };
    QCheckBox* unacknowledged_filter_{ nullptr };
    QPushButton* acknowledge_button_{ nullptr };
    QPushButton* acknowledge_all_button_{ nullptr };
    QTableWidget* table_{ nullptr };
    OperationalLogState state_;
    AcknowledgeHandler acknowledge_handler_;
    AcknowledgeAllHandler acknowledge_all_handler_;
};

}  // namespace logistics::control_center
