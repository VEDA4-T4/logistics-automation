#pragma once

#include <QList>
#include <QMap>
#include <QWidget>

#include "logistics/control_center/operations_dashboard_state.hpp"

class QFrame;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QTimer;

namespace logistics::control_center {

class FactoryTopViewWidget;

class OperationsDashboardPanel final : public QWidget {
    Q_OBJECT

public:
    explicit OperationsDashboardPanel(QWidget* parent = nullptr);

    void setState(const OperationsDashboardState& state);
    void setMqttConnected(bool connected);
    void setControlTarget(const QString& target_device_id);

signals:
    void controlTargetSelected(const QString& target_device_id, const QString& display_name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct ProcessCardWidgets {
        QFrame* card{ nullptr };
        QLabel* status{ nullptr };
        QLabel* current_state{ nullptr };
        QLabel* work_or_error{ nullptr };
        QLabel* device_and_updated_at{ nullptr };
        QMap<int, QLabel*> sensor_indicators;
    };

    ProcessCardWidgets createProcessCard(const ProcessUnitStatus& process);
    void rebuildProcessCards();
    void refreshOverall();
    void refreshProcesses();
    void refreshTimestamps();
    void refreshControlTargetSelection();

    QFrame* overall_card_{ nullptr };
    QLabel* live_status_{ nullptr };
    QLabel* overall_status_{ nullptr };
    QLabel* overall_summary_{ nullptr };
    QLabel* overall_work_count_{ nullptr };
    QLabel* overall_detail_{ nullptr };
    QLabel* overall_updated_at_{ nullptr };
    FactoryTopViewWidget* factory_top_view_{ nullptr };
    QGridLayout* process_layout_{ nullptr };
    QTimer* timestamp_timer_{ nullptr };
    ProcessDashboardStatus overall_;
    QList<ProcessUnitStatus> processes_;
    QMap<QString, ProcessCardWidgets> process_cards_;
    QString selected_control_target_{ QStringLiteral("SYSTEM") };
};

}  // namespace logistics::control_center
