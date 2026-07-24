#pragma once

#include <QList>
#include <QMap>
#include <QWidget>

#include "logistics/control_center/operations_dashboard_state.hpp"

class QFrame;
class QHBoxLayout;
class QLabel;
class QTimer;

namespace logistics::control_center {

class OperationsDashboardPanel final : public QWidget {
public:
    explicit OperationsDashboardPanel(QWidget* parent = nullptr);

    void setState(const OperationsDashboardState& state);
    void setMqttConnected(bool connected);

private:
    struct ProcessCardWidgets {
        QFrame* card{ nullptr };
        QLabel* status{ nullptr };
        QLabel* current_state{ nullptr };
        QLabel* work_or_error{ nullptr };
        QLabel* device_and_updated_at{ nullptr };
    };

    ProcessCardWidgets createProcessCard(const ProcessUnitStatus& process);
    void rebuildProcessCards();
    void refreshOverall();
    void refreshProcesses();
    void refreshTimestamps();

    QLabel* live_status_{ nullptr };
    QLabel* overall_status_{ nullptr };
    QLabel* overall_summary_{ nullptr };
    QLabel* overall_work_count_{ nullptr };
    QLabel* overall_detail_{ nullptr };
    QLabel* overall_updated_at_{ nullptr };
    QHBoxLayout* process_layout_{ nullptr };
    QTimer* timestamp_timer_{ nullptr };
    ProcessDashboardStatus overall_;
    QList<ProcessUnitStatus> processes_;
    QMap<QString, ProcessCardWidgets> process_cards_;
};

}  // namespace logistics::control_center
