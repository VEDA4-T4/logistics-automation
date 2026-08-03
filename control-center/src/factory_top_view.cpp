#include "logistics/control_center/factory_top_view.hpp"

namespace logistics::control_center {
namespace {

QString NormalizedState(const QString& current_state) {
    return current_state.trimmed().toUpper();
}

FactoryMotionPhase MotionPhaseFor(const ProcessUnitStatus& process, const QString& state) {
    if (state == QStringLiteral("VISION_PROCESSING")) {
        return FactoryMotionPhase::VisionProcessing;
    }
    if (state == QStringLiteral("PICKING")) {
        return FactoryMotionPhase::GripperPick;
    }
    if (state == QStringLiteral("TRANSFERRING")) {
        return FactoryMotionPhase::GripperTransfer;
    }
    if (state == QStringLiteral("PLACED")) {
        return FactoryMotionPhase::GripperPlaced;
    }
    if (state == QStringLiteral("COMPLETED")) {
        return process.key == QString::fromLatin1(kLineTracerProcessKey) ? FactoryMotionPhase::LineCompleted
                                                                         : FactoryMotionPhase::GripperPlaced;
    }
    if (state == QStringLiteral("SORTING") || state == QStringLiteral("ROUTING")) {
        for (const auto& sensor : process.sensors) {
            if (NormalizedState(sensor.measurement_status) != QStringLiteral("DETECTED")) {
                continue;
            }
            if (sensor.sensor_id == 1) {
                return FactoryMotionPhase::SortingSensor1;
            }
            if (sensor.sensor_id == 2) {
                return FactoryMotionPhase::SortingSensor2;
            }
            if (sensor.sensor_id == 3) {
                return FactoryMotionPhase::SortingSensor3;
            }
        }
        return FactoryMotionPhase::SortingConveyor;
    }
    if (state == QStringLiteral("DELIVERING") || state == QStringLiteral("FOLLOWING_LINE")) {
        return FactoryMotionPhase::LineFollowing;
    }
    return FactoryMotionPhase::InputConveyor;
}

bool IsWorkingState(const QString& state) {
    return state == QStringLiteral("VISION_PROCESSING") || state == QStringLiteral("PICKING") ||
           state == QStringLiteral("TRANSFERRING") || state == QStringLiteral("PLACED") ||
           state == QStringLiteral("COMPLETED") || state == QStringLiteral("SORTING") ||
           state == QStringLiteral("ROUTING") || state == QStringLiteral("DELIVERING") ||
           state == QStringLiteral("FOLLOWING_LINE");
}

FactoryNodeVisual BaseVisual(const ProcessUnitStatus& process) {
    FactoryNodeVisual visual;
    visual.process_key = process.key;
    visual.device_id = process.device_id;
    visual.work_id = process.work_id;
    visual.state_text = process.current_state;
    visual.sensors.reserve(process.sensors.size());
    for (const auto& sensor : process.sensors) {
        const auto status = NormalizedState(sensor.measurement_status);
        visual.sensors.append({ .sensor_id = sensor.sensor_id,
                                .detected = status == QStringLiteral("DETECTED"),
                                .fault = status == QStringLiteral("FAULT"),
                                .distance_text = FactoryDistanceText(sensor.distance_cm) });
    }
    return visual;
}

FactoryNodeVisual DisconnectedVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::Disconnected;
    visual.opacity = 0.15;
    return visual;
}

FactoryNodeVisual EmergencyVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::EmergencyStop;
    visual.opacity = 1.0;
    return visual;
}

FactoryNodeVisual ErrorVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::Error;
    visual.opacity = 1.0;
    return visual;
}

FactoryNodeVisual WorkingVisual(const ProcessUnitStatus& process, const QString& state) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::Working;
    visual.motion_phase = MotionPhaseFor(process, state);
    visual.opacity = 1.0;
    visual.motion_enabled = true;
    return visual;
}

FactoryNodeVisual RunningVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::Running;
    visual.opacity = 1.0;
    return visual;
}

FactoryNodeVisual WaitingVisual(const ProcessUnitStatus& process) {
    return BaseVisual(process);
}

}  // namespace

QString FactoryDistanceText(int distance_cm) {
    return distance_cm >= 0 ? QStringLiteral("%1 cm").arg(distance_cm) : QStringLiteral("-- cm");
}

std::optional<int> FactoryRouteIndex(const QString& current_state) {
    auto state = NormalizedState(current_state);
    state.replace(QLatin1Char('-'), QLatin1Char('_'));
    for (int route = 1; route <= 3; ++route) {
        const auto suffix = QString::number(route);
        if (state == QStringLiteral("ROUTE_") + suffix || state == QStringLiteral("DESTINATION_") + suffix ||
            state == QStringLiteral("START_") + suffix) {
            return route;
        }
    }
    return std::nullopt;
}

FactoryNodeVisual BuildFactoryNodeVisual(const ProcessUnitStatus& process) {
    const auto state = NormalizedState(process.current_state);
    if (process.connection_state != logistics::contracts::mqtt::ConnectionState::kOnline ||
        state == QStringLiteral("DISCONNECTED")) {
        return DisconnectedVisual(process);
    }
    if (state == QStringLiteral("EMERGENCY_STOP") || state == QStringLiteral("ESTOP")) {
        return EmergencyVisual(process);
    }
    if (process.has_error) {
        return ErrorVisual(process);
    }
    if (!process.work_id.isEmpty() && IsWorkingState(state)) {
        return WorkingVisual(process, state);
    }
    if (state == QStringLiteral("RUNNING") || state == QStringLiteral("ONLINE")) {
        return RunningVisual(process);
    }
    return WaitingVisual(process);
}

}  // namespace logistics::control_center
