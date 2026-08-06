#pragma once

#include <QColor>
#include <QGraphicsView>
#include <QList>
#include <QPointF>
#include <QString>
#include <memory>
#include <optional>

#include "logistics/control_center/operations_dashboard_state.hpp"

class QResizeEvent;

namespace logistics::control_center {

enum class FactoryNodeVisualState { Disconnected, EmergencyStop, Error, Working, Running, Waiting };
enum class FactoryMotionPhase {
    None,
    InputConveyor,
    InputDetected,
    VisionProcessing,
    GripperPick,
    GripperTransfer,
    GripperPlaced,
    SortingConveyor,
    SortingSensor1,
    SortingSensor2,
    SortingSensor3,
    LineStart,
    LineFollowing,
    LineCompleted
};

struct FactorySensorVisual {
    int sensor_id{ 0 };
    bool detected{ false };
    bool fault{ false };
    QString distance_text{ QStringLiteral("-- cm") };
};

struct FactoryNodeVisual {
    QString process_key;
    QString device_id;
    QString work_id;
    QString state_text;
    FactoryNodeVisualState state{ FactoryNodeVisualState::Waiting };
    FactoryMotionPhase motion_phase{ FactoryMotionPhase::None };
    QList<FactorySensorVisual> sensors;
    qreal opacity{ 0.4 };
    bool motion_enabled{ false };
};

[[nodiscard]] QString FactoryDistanceText(int distance_cm);
[[nodiscard]] std::optional<int> FactoryRouteIndex(const QString& current_state);
[[nodiscard]] FactoryNodeVisual BuildFactoryNodeVisual(const ProcessUnitStatus& process);

class FactoryTopViewWidget final : public QGraphicsView {
    Q_OBJECT

public:
    explicit FactoryTopViewWidget(QWidget* parent = nullptr);
    ~FactoryTopViewWidget() override;

    void setProcesses(const QList<ProcessUnitStatus>& processes);
    void setSelectedDeviceId(const QString& device_id);
    [[nodiscard]] QString selectedDeviceId() const;
    [[nodiscard]] qreal nodeOpacity(const QString& process_key) const;
    [[nodiscard]] QColor nodeColor(const QString& process_key) const;
    [[nodiscard]] QString sensorText(const QString& process_key, int sensor_id) const;
    [[nodiscard]] QPointF boxPosition(const QString& process_key) const;
    [[nodiscard]] qreal gripperAngle() const;
    [[nodiscard]] bool gripperProductVisible() const;
    [[nodiscard]] QPointF gripperProductPosition() const;
    [[nodiscard]] qreal sortingServoAngle() const;
    void advanceAnimationsForTest();
    void selectProcessForTest(const QString& process_key);

signals:
    void controlTargetSelected(const QString& device_id, const QString& display_name);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void selectProcess(const QString& process_key);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace logistics::control_center
