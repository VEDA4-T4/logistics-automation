#pragma once

#include <QList>
#include <QString>
#include <optional>

#include "logistics/control_center/operations_dashboard_state.hpp"

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

}  // namespace logistics::control_center
