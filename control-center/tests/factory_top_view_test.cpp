#include "logistics/control_center/factory_top_view.hpp"

#include <QApplication>
#include <QColor>
#include <QGraphicsScene>
#include <QPointF>
#include <QRectF>
#include <cassert>
#include <utility>

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
    active.current_state = QStringLiteral("  transferring  ");
    active.has_error = false;
    visual = BuildFactoryNodeVisual(active);
    assert(visual.state == FactoryNodeVisualState::Working);
    assert(visual.motion_enabled);
    assert(visual.motion_phase == FactoryMotionPhase::GripperTransfer);

    ProcessUnitStatus estop = active;
    estop.current_state = QStringLiteral("  estop  ");
    visual = BuildFactoryNodeVisual(estop);
    assert(visual.state == FactoryNodeVisualState::EmergencyStop);
    assert(!visual.motion_enabled);

    ProcessUnitStatus error = active;
    error.has_error = true;
    visual = BuildFactoryNodeVisual(error);
    assert(visual.state == FactoryNodeVisualState::Error);
    assert(!visual.motion_enabled);

    ProcessUnitStatus sorting;
    sorting.key = QStringLiteral("sorting");
    sorting.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    sorting.current_state = QStringLiteral("ROUTING");
    sorting.work_id = QStringLiteral("WORK-2");
    sorting.sensors.append({ .sensor_id = 1,
                             .display_name = QStringLiteral("S1"),
                             .measurement_status = QStringLiteral("DETECTED"),
                             .distance_cm = 21,
                             .updated_at = {} });
    visual = BuildFactoryNodeVisual(sorting);
    assert(visual.state == FactoryNodeVisualState::Working);
    assert(visual.motion_enabled);
    assert(visual.motion_phase == FactoryMotionPhase::SortingSensor1);
    sorting.sensors.front().sensor_id = 2;
    visual = BuildFactoryNodeVisual(sorting);
    assert(visual.motion_phase == FactoryMotionPhase::SortingSensor2);
    sorting.sensors.front().sensor_id = 3;
    visual = BuildFactoryNodeVisual(sorting);
    assert(visual.motion_phase == FactoryMotionPhase::SortingSensor3);

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
    assert(FactoryRouteIndex(QStringLiteral("  route-1  ")) == 1);
    assert(FactoryRouteIndex(QStringLiteral("destination-2")) == 2);
    assert(FactoryRouteIndex(QStringLiteral("start-3")) == 3);
    assert(FactoryRouteIndex(QStringLiteral("1")) == 1);
    assert(FactoryRouteIndex(QStringLiteral("2")) == 2);
    assert(FactoryRouteIndex(QStringLiteral("3")) == 3);
    assert(FactoryRouteIndex(QStringLiteral("4")) == std::nullopt);
    assert(FactoryRouteIndex(QStringLiteral("DELIVERING")) == std::nullopt);

    auto Process = [](QString key, QString id, QString state, QString work) {
        ProcessUnitStatus process;
        process.key = std::move(key);
        process.device_id = std::move(id);
        process.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
        process.current_state = std::move(state);
        process.work_id = std::move(work);
        return process;
    };
    auto input = Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-01"), QStringLiteral("RUNNING"),
                         QStringLiteral("WORK-IN"));
    auto vision = Process(QStringLiteral("vision"), QStringLiteral("PI-VISION-01"), QStringLiteral("VISION_PROCESSING"),
                          QStringLiteral("WORK-VISION"));
    auto gripper = Process(QStringLiteral("gripper"), QStringLiteral("PI-GRIPPER-01"), QStringLiteral("TRANSFERRING"),
                           QStringLiteral("WORK-GRIPPER"));
    sorting = Process(QStringLiteral("sorting"), QStringLiteral("PI-SORTING-01"), QStringLiteral("SORTING"),
                      QStringLiteral("WORK-SORT"));
    sorting.sensors.append({ .sensor_id = 2,
                             .display_name = QStringLiteral("S2"),
                             .measurement_status = QStringLiteral("DETECTED"),
                             .distance_cm = 11,
                             .updated_at = {} });
    auto line_tracer = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-01"), QStringLiteral("DISCONNECTED"),
                               QStringLiteral("WORK-LT"));

    logistics::control_center::FactoryTopViewWidget view;
    view.resize(700, 500);
    view.setProcesses({ input, vision, gripper, sorting, line_tracer });
    view.show();
    application.processEvents();
    const auto scene_item_count = view.scene()->items().size();

    assert(view.sceneRect() == QRectF(0, 0, 700, 500));
    assert(view.nodeOpacity(QStringLiteral("input")) == 1.0);
    assert(view.nodeOpacity(QStringLiteral("linetracer")) == 0.15);
    assert(view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#89d185")));
    assert(view.nodeColor(QStringLiteral("linetracer")) == QColor(QStringLiteral("#777777")));
    assert(view.sensorText(QStringLiteral("sorting"), 2) == QStringLiteral("11 cm"));
    assert(view.gripperAngle() == 90.0);

    input.work_id = QStringLiteral("WORK-GRIPPER");
    gripper.current_state = QStringLiteral("PICKING");
    view.setProcesses({ input, gripper });
    assert(view.gripperProductVisible());
    assert(view.gripperProductPosition() == QPointF(440, 103));
    gripper.current_state = QStringLiteral("TRANSFERRING");
    view.setProcesses({ input, gripper });
    assert(view.gripperProductVisible());
    assert(view.gripperProductPosition() == QPointF(482, 145));
    gripper.current_state = QStringLiteral("PLACED");
    sorting.work_id = QStringLiteral("WORK-GRIPPER");
    view.setProcesses({ input, gripper, sorting });
    assert(view.gripperProductVisible());
    assert(view.gripperProductPosition() == QPointF(525, 126));

    gripper.work_id = QStringLiteral("WORK-MISMATCH");
    view.setProcesses({ input, gripper, sorting });
    assert(!view.gripperProductVisible());
    gripper.work_id = QStringLiteral("WORK-GRIPPER");
    gripper.connection_state = logistics::contracts::mqtt::ConnectionState::kOffline;
    view.setProcesses({ input, gripper, sorting });
    assert(!view.gripperProductVisible());
    gripper.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;

    line_tracer.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    line_tracer.current_state = QStringLiteral("FOLLOWING_LINE");
    line_tracer.work_id = QStringLiteral("WORK-ROUTE");
    sorting.work_id = QStringLiteral("WORK-ROUTE");
    const struct {
        QString destination;
        qreal servo_angle;
        QPointF line_start;
    } route_cases[] = {
        { QStringLiteral("1"), 0.0, QPointF(500, 250) },
        { QStringLiteral("route-2"), 45.0, QPointF(500, 345) },
        { QStringLiteral("destination-3"), 90.0, QPointF(500, 442) },
    };
    for (const auto& route_case : route_cases) {
        sorting.destination = route_case.destination;
        view.setProcesses({ sorting, line_tracer });
        assert(view.sortingServoAngle() == route_case.servo_angle);
        assert(view.boxPosition(QStringLiteral("linetracer")) == route_case.line_start);
    }
    sorting.destination.clear();
    const auto unrouted_servo_angle = view.sortingServoAngle();
    const auto unrouted_line_position = view.boxPosition(QStringLiteral("linetracer"));
    view.setProcesses({ sorting, line_tracer });
    view.advanceAnimationsForTest();
    assert(view.sortingServoAngle() == unrouted_servo_angle);
    assert(view.boxPosition(QStringLiteral("linetracer")) == unrouted_line_position);
    sorting.destination = QStringLiteral("2");
    line_tracer.work_id = QStringLiteral("WORK-OTHER");
    view.setProcesses({ sorting, line_tracer });
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("linetracer")) == unrouted_line_position);

    auto stale_sorting = sorting;
    stale_sorting.connection_state = logistics::contracts::mqtt::ConnectionState::kOffline;
    view.setProcesses({ stale_sorting });
    assert(view.sensorText(QStringLiteral("sorting"), 2) == QStringLiteral("-- cm"));
    view.setProcesses({ sorting });

    const auto input_before = view.boxPosition(QStringLiteral("input"));
    const auto sorting_pinned = view.boxPosition(QStringLiteral("sorting"));
    assert(sorting_pinned == QPointF(525, 178));
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("input")) != input_before);
    assert(view.boxPosition(QStringLiteral("sorting")) == sorting_pinned);

    input.current_state = QStringLiteral("STOPPED");
    sorting.sensors.clear();
    view.setProcesses({ input, vision, gripper, sorting, line_tracer });
    assert(view.sensorText(QStringLiteral("sorting"), 2) == QStringLiteral("-- cm"));
    const auto input_stopped = view.boxPosition(QStringLiteral("input"));
    const auto sorting_before = view.boxPosition(QStringLiteral("sorting"));
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("input")) == input_stopped);
    assert(view.boxPosition(QStringLiteral("sorting")) != sorting_before);

    line_tracer.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    line_tracer.current_state = QStringLiteral("FOLLOWING_LINE");
    line_tracer.work_id = QStringLiteral("WORK-LT");
    sorting.work_id = QStringLiteral("WORK-LT");
    sorting.destination = QStringLiteral("2");
    view.setProcesses({ sorting, line_tracer });
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(500, 345));
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(292, 345));
    line_tracer.current_state = QStringLiteral("COMPLETED");
    view.setProcesses({ sorting, line_tracer });
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(58, 345));
    view.advanceAnimationsForTest();
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(58, 345));

    logistics::control_center::FactoryTopViewWidget unrouted_view;
    auto unrouted_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-02"),
                                 QStringLiteral("FOLLOWING_LINE"), QStringLiteral("WORK-WITHOUT-ROUTE"));
    const auto unrouted_position = unrouted_view.boxPosition(QStringLiteral("linetracer"));
    unrouted_view.setProcesses({ unrouted_line });
    unrouted_view.advanceAnimationsForTest();
    assert(unrouted_view.boxPosition(QStringLiteral("linetracer")) == unrouted_position);

    logistics::control_center::FactoryTopViewWidget disconnected_view;
    auto disconnected_input = Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-02"), QStringLiteral("RUNNING"),
                                      QStringLiteral("WORK-STALE-IN"));
    disconnected_input.connection_state = logistics::contracts::mqtt::ConnectionState::kOffline;
    disconnected_input.sensors.append({ .sensor_id = 1,
                                        .display_name = QStringLiteral("S1"),
                                        .measurement_status = QStringLiteral("DETECTED"),
                                        .distance_cm = 7,
                                        .updated_at = {} });
    auto disconnected_sorting = Process(QStringLiteral("sorting"), QStringLiteral("PI-SORTING-02"),
                                        QStringLiteral("SORTING"), QStringLiteral("WORK-STALE-SORT"));
    disconnected_sorting.connection_state = logistics::contracts::mqtt::ConnectionState::kOffline;
    disconnected_sorting.sensors.append({ .sensor_id = 2,
                                          .display_name = QStringLiteral("S2"),
                                          .measurement_status = QStringLiteral("DETECTED"),
                                          .distance_cm = 9,
                                          .updated_at = {} });
    auto disconnected_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-03"),
                                     QStringLiteral("FOLLOWING_LINE"), QStringLiteral("ROUTE_2"));
    disconnected_line.connection_state = logistics::contracts::mqtt::ConnectionState::kOffline;
    const auto disconnected_input_position = disconnected_view.boxPosition(QStringLiteral("input"));
    const auto disconnected_sorting_position = disconnected_view.boxPosition(QStringLiteral("sorting"));
    const auto disconnected_line_position = disconnected_view.boxPosition(QStringLiteral("linetracer"));
    disconnected_view.setProcesses({ disconnected_input, disconnected_sorting, disconnected_line });
    assert(disconnected_view.boxPosition(QStringLiteral("input")) == disconnected_input_position);
    assert(disconnected_view.boxPosition(QStringLiteral("sorting")) == disconnected_sorting_position);
    assert(disconnected_view.boxPosition(QStringLiteral("linetracer")) == disconnected_line_position);

    const bool animation_preference = QApplication::isEffectEnabled(Qt::UI_AnimateCombo);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    logistics::control_center::FactoryTopViewWidget reduced_motion_view;
    reduced_motion_view.setProcesses({ vision });
    reduced_motion_view.advanceAnimationsForTest();
    reduced_motion_view.advanceAnimationsForTest();
    assert(reduced_motion_view.nodeOpacity(QStringLiteral("vision")) == 1.0);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, animation_preference);

    QString selected;
    QObject::connect(&view, &logistics::control_center::FactoryTopViewWidget::controlTargetSelected,
                     [&selected](const QString& id, const QString&) { selected = id; });
    view.selectProcessForTest(QStringLiteral("vision"));
    assert(selected == QStringLiteral("PI-VISION-01"));
    view.setSelectedDeviceId(QStringLiteral("PI-VISION-01"));
    assert(view.selectedDeviceId() == QStringLiteral("PI-VISION-01"));
    assert(view.scene()->items().size() == scene_item_count);
}
