#include "logistics/control_center/factory_top_view.hpp"

#include <QApplication>
#include <cassert>

using logistics::control_center::BuildFactoryNodeVisual;
using logistics::control_center::FactoryDistanceText;
using logistics::control_center::FactoryMotionPhase;
using logistics::control_center::FactoryNodeVisualState;
using logistics::control_center::FactoryRouteIndex;
using logistics::control_center::ProcessUnitStatus;

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    ProcessUnitStatus disconnected;
    disconnected.key = QStringLiteral("input");
    disconnected.current_state = QStringLiteral("DISCONNECTED");
    disconnected.work_id = QStringLiteral("WORK-1");
    auto visual = BuildFactoryNodeVisual(disconnected);
    assert(visual.state == FactoryNodeVisualState::Disconnected);
    assert(!visual.motion_enabled);
    assert(visual.opacity == 0.15);

    ProcessUnitStatus emergency = disconnected;
    emergency.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    emergency.current_state = QStringLiteral("EMERGENCY_STOP");
    visual = BuildFactoryNodeVisual(emergency);
    assert(visual.state == FactoryNodeVisualState::EmergencyStop);
    assert(!visual.motion_enabled);

    ProcessUnitStatus active = emergency;
    active.current_state = QStringLiteral("TRANSFERRING");
    active.has_error = false;
    visual = BuildFactoryNodeVisual(active);
    assert(visual.state == FactoryNodeVisualState::Working);
    assert(visual.motion_enabled);
    assert(visual.motion_phase == FactoryMotionPhase::GripperTransfer);

    ProcessUnitStatus sensor_node;
    sensor_node.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    sensor_node.current_state = QStringLiteral("STOPPED");
    sensor_node.sensors.append({ .sensor_id = 2,
                                 .display_name = QStringLiteral("S2"),
                                 .measurement_status = QStringLiteral("DETECTED"),
                                 .distance_cm = 11,
                                 .updated_at = {} });
    visual = BuildFactoryNodeVisual(sensor_node);
    assert(!visual.motion_enabled);
    assert(visual.sensors.front().detected);
    assert(visual.sensors.front().distance_text == QStringLiteral("11 cm"));
    assert(FactoryDistanceText(-1) == QStringLiteral("-- cm"));
    assert(FactoryRouteIndex(QStringLiteral("ROUTE_2")) == 2);
    assert(FactoryRouteIndex(QStringLiteral("DELIVERING")) == std::nullopt);
}
