#include "logistics/control_center/factory_top_view.hpp"

#include <QApplication>
#include <QColor>
#include <QGraphicsLineItem>
#include <QGraphicsObject>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QKeyEvent>
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <cassert>
#include <utility>

using logistics::control_center::BuildFactoryNodeVisual;
using logistics::control_center::FactoryDistanceText;
using logistics::control_center::FactoryMotionPhase;
using logistics::control_center::FactoryNodeColor;
using logistics::control_center::FactoryNodeVisualState;
using logistics::control_center::FactoryRouteIndex;
using logistics::control_center::LineTracerPositionStatus;
using logistics::control_center::ProcessUnitStatus;

namespace {

QList<QGraphicsRectItem*> VisibleProducts(const logistics::control_center::FactoryTopViewWidget& view) {
    QList<QGraphicsRectItem*> products;
    for (auto* item : view.scene()->items()) {
        auto* rectangle = dynamic_cast<QGraphicsRectItem*>(item);
        if (rectangle != nullptr && rectangle->rect() == QRectF(-8, -8, 16, 16) &&
            rectangle->brush().color() == QColor(QStringLiteral("#ce9178")) && rectangle->isVisible()) {
            products.append(rectangle);
        }
    }
    return products;
}

}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setDesktopSettingsAware(false);
    QApplication application(argc, argv);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, true);

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

    auto emergency_with_error = emergency;
    emergency_with_error.has_error = true;
    assert(BuildFactoryNodeVisual(emergency_with_error).state == FactoryNodeVisualState::EmergencyStop);
    assert(FactoryNodeColor(FactoryNodeVisualState::EmergencyStop) != FactoryNodeColor(FactoryNodeVisualState::Error));

    ProcessUnitStatus active = emergency;
    active.key = QStringLiteral("gripper");
    active.current_state = QStringLiteral("  transferring  ");
    active.has_error = false;
    visual = BuildFactoryNodeVisual(active);
    assert(visual.state == FactoryNodeVisualState::Working);
    assert(visual.motion_enabled);
    assert(visual.motion_phase == FactoryMotionPhase::GripperTransfer);

    ProcessUnitStatus ready_gripper = active;
    ready_gripper.current_state = QStringLiteral("READY");
    ready_gripper.work_id.clear();
    visual = BuildFactoryNodeVisual(ready_gripper);
    assert(visual.state == FactoryNodeVisualState::Waiting);
    assert(!visual.motion_enabled);

    ProcessUnitStatus inactive_completion = active;
    inactive_completion.key = QStringLiteral("sorting");
    inactive_completion.current_state = QStringLiteral("CYCLE_COMPLETE");
    visual = BuildFactoryNodeVisual(inactive_completion);
    assert(visual.state == FactoryNodeVisualState::Waiting);
    assert(!visual.motion_enabled);
    inactive_completion.current_state = QStringLiteral("HOME");
    assert(BuildFactoryNodeVisual(inactive_completion).state == FactoryNodeVisualState::Waiting);
    inactive_completion.key = QStringLiteral("input");
    inactive_completion.current_state = QStringLiteral("COMPLETED");
    assert(BuildFactoryNodeVisual(inactive_completion).state == FactoryNodeVisualState::Waiting);
    inactive_completion.key = QStringLiteral("vision");
    assert(BuildFactoryNodeVisual(inactive_completion).state == FactoryNodeVisualState::Waiting);

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

    ProcessUnitStatus recovery = active;
    recovery.current_state = QStringLiteral("RECOVERY");
    recovery.has_error = false;
    visual = BuildFactoryNodeVisual(recovery);
    assert(visual.state == FactoryNodeVisualState::Recovery);
    assert(FactoryNodeColor(visual.state) == QColor(QStringLiteral("#75beff")));

    ProcessUnitStatus stopped = active;
    stopped.current_state = QStringLiteral("STOPPED");
    stopped.has_error = false;
    visual = BuildFactoryNodeVisual(stopped);
    assert(visual.state == FactoryNodeVisualState::Stopped);
    assert(FactoryNodeColor(visual.state) == QColor(QStringLiteral("#cca700")));

    ProcessUnitStatus lifecycle = active;
    lifecycle.key = QStringLiteral("linetracer");
    for (const auto& state :
         { QStringLiteral("PICKUP_READY_A"), QStringLiteral("ARRIVED_B"), QStringLiteral("UNLOADING_C") }) {
        lifecycle.current_state = state;
        visual = BuildFactoryNodeVisual(lifecycle);
        assert(visual.state == FactoryNodeVisualState::Working);
    }
    lifecycle.current_state = QStringLiteral("PICKUP_READY_AB");
    visual = BuildFactoryNodeVisual(lifecycle);
    assert(visual.state == FactoryNodeVisualState::Waiting);
    assert(visual.opacity == 1.0);
    lifecycle.current_state = QStringLiteral("POSITION_UNKNOWN");
    visual = BuildFactoryNodeVisual(lifecycle);
    assert(visual.state == FactoryNodeVisualState::Error);
    lifecycle.current_state = QStringLiteral("FUTURE_STATE");
    visual = BuildFactoryNodeVisual(lifecycle);
    assert(visual.state == FactoryNodeVisualState::Waiting);
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

    auto assigned_vision = Process(QStringLiteral("vision"), QStringLiteral("PI-VISION-01"),
                                   QStringLiteral("WORK_ASSIGNED"), QStringLiteral("WORK-VISION"));
    visual = BuildFactoryNodeVisual(assigned_vision);
    assert(visual.state == FactoryNodeVisualState::Working);
    assert(visual.motion_enabled);
    assert(visual.motion_phase == FactoryMotionPhase::VisionProcessing);
    assigned_vision.work_id.clear();
    visual = BuildFactoryNodeVisual(assigned_vision);
    assert(visual.state == FactoryNodeVisualState::Working);
    assert(!visual.motion_enabled);
    assigned_vision.current_state = QStringLiteral("AWAITING_WORK_ID");
    assert(BuildFactoryNodeVisual(assigned_vision).state == FactoryNodeVisualState::Working);
    assigned_vision.current_state = QStringLiteral("WAITING_FOR_PRODUCT");
    assert(BuildFactoryNodeVisual(assigned_vision).state == FactoryNodeVisualState::Running);

    auto actual_sorting = Process(QStringLiteral("sorting"), QStringLiteral("PI-SORTING-01"),
                                  QStringLiteral("GATE_MOVING_DEST_2"), QStringLiteral("WORK-SORT"));
    assert(BuildFactoryNodeVisual(actual_sorting).state == FactoryNodeVisualState::Working);
    actual_sorting.current_state = QStringLiteral("WAITING_ITEM_DEST_2");
    assert(BuildFactoryNodeVisual(actual_sorting).state == FactoryNodeVisualState::Working);
    actual_sorting.current_state = QStringLiteral("GATE_MOVING_DEST_20");
    assert(BuildFactoryNodeVisual(actual_sorting).state == FactoryNodeVisualState::Waiting);
    actual_sorting.current_state = QStringLiteral("GATE_MOVING_DEST_2");
    auto actual_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-01"), QStringLiteral("CORRECTING"),
                               QStringLiteral("WORK-LINE"));
    assert(BuildFactoryNodeVisual(actual_line).state == FactoryNodeVisualState::Working);

    for (const auto& wrong_key : { QStringLiteral("input"), QStringLiteral("sorting"), QStringLiteral("gripper"),
                                   QStringLiteral("linetracer") }) {
        for (const auto& vision_state : { QStringLiteral("WORK_ASSIGNED"), QStringLiteral("AWAITING_WORK_ID"),
                                          QStringLiteral("WAITING_FOR_PRODUCT") }) {
            const auto wrong_process =
                Process(wrong_key, QStringLiteral("WRONG-NODE"), vision_state, QStringLiteral("WORK-WRONG"));
            const auto wrong_visual = BuildFactoryNodeVisual(wrong_process);
            const bool valid_gripper_assignment =
                wrong_key == QStringLiteral("gripper") && vision_state == QStringLiteral("WORK_ASSIGNED");
            assert(wrong_visual.state ==
                   (valid_gripper_assignment ? FactoryNodeVisualState::Working : FactoryNodeVisualState::Waiting));
            assert(wrong_visual.motion_enabled == valid_gripper_assignment);
        }
    }

    auto busy_input =
        Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-BUSY"), QStringLiteral("BUSY"), QStringLiteral("W"));
    assert(BuildFactoryNodeVisual(busy_input).state == FactoryNodeVisualState::Working);
    actual_sorting.current_state = QStringLiteral("BUSY");
    assert(BuildFactoryNodeVisual(actual_sorting).state == FactoryNodeVisualState::Working);
    actual_sorting.current_state = QStringLiteral("GATE_MOVING_DEST_2");

    logistics::control_center::FactoryTopViewWidget cross_process_view;
    auto wrong_input = Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-WRONG"),
                               QStringLiteral("WORK_ASSIGNED"), QStringLiteral("WORK-WRONG-INPUT"));
    auto wrong_sorting = Process(QStringLiteral("sorting"), QStringLiteral("PI-SORTING-WRONG"),
                                 QStringLiteral("WORK_ASSIGNED"), QStringLiteral("WORK-WRONG-SORTING"));
    auto wrong_gripper = Process(QStringLiteral("gripper"), QStringLiteral("PI-GRIPPER-WRONG"),
                                 QStringLiteral("WORK_ASSIGNED"), QStringLiteral("WORK-WRONG-GRIPPER"));
    auto wrong_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-WRONG"),
                              QStringLiteral("WORK_ASSIGNED"), QStringLiteral("WORK-WRONG-LINE"));
    cross_process_view.setProcesses({ wrong_input, wrong_sorting, wrong_gripper, wrong_line });
    const auto wrong_input_position = cross_process_view.boxPosition(QStringLiteral("input"));
    const auto wrong_sorting_position = cross_process_view.boxPosition(QStringLiteral("sorting"));
    const auto wrong_line_position = cross_process_view.boxPosition(QStringLiteral("linetracer"));
    cross_process_view.advanceAnimationsForTest();
    assert(cross_process_view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#ffffff")));
    assert(cross_process_view.nodeColor(QStringLiteral("sorting")) == QColor(QStringLiteral("#ffffff")));
    assert(cross_process_view.nodeColor(QStringLiteral("gripper")) == QColor(QStringLiteral("#75beff")));
    assert(cross_process_view.nodeColor(QStringLiteral("linetracer")) == QColor(QStringLiteral("#ffffff")));
    assert(cross_process_view.boxPosition(QStringLiteral("input")) == wrong_input_position);
    assert(cross_process_view.boxPosition(QStringLiteral("sorting")) == wrong_sorting_position);
    assert(cross_process_view.boxPosition(QStringLiteral("linetracer")) == wrong_line_position);
    assert(VisibleProducts(cross_process_view).isEmpty());

    logistics::control_center::FactoryTopViewWidget no_work_view;
    auto no_work_input =
        Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-01"), QStringLiteral("RUNNING"), QString{});
    actual_sorting.work_id.clear();
    assigned_vision.current_state = QStringLiteral("AWAITING_WORK_ID");
    no_work_view.setProcesses({ no_work_input, assigned_vision, actual_sorting });
    assert(no_work_view.nodeColor(QStringLiteral("vision")) == QColor(QStringLiteral("#75beff")));
    assert(VisibleProducts(no_work_view).isEmpty());
    const auto no_work_input_position = no_work_view.boxPosition(QStringLiteral("input"));
    const auto no_work_sorting_position = no_work_view.boxPosition(QStringLiteral("sorting"));
    no_work_view.advanceAnimationsForTest();
    assert(no_work_view.nodeOpacity(QStringLiteral("vision")) < 1.0);
    assert(no_work_view.boxPosition(QStringLiteral("input")) == no_work_input_position);
    assert(no_work_view.boxPosition(QStringLiteral("sorting")) == no_work_sorting_position);
    assert(VisibleProducts(no_work_view).isEmpty());

    assigned_vision.has_error = true;
    no_work_view.setProcesses({ assigned_vision });
    assert(no_work_view.nodeColor(QStringLiteral("vision")) == QColor(QStringLiteral("#f14c4c")));
    assigned_vision.has_error = false;
    no_work_view.setProcesses({ assigned_vision });
    assert(no_work_view.nodeColor(QStringLiteral("vision")) == QColor(QStringLiteral("#75beff")));
    no_work_view.advanceAnimationsForTest();
    assert(no_work_view.nodeOpacity(QStringLiteral("vision")) < 1.0);

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

    auto concurrent_input = input;
    concurrent_input.work_id = QStringLiteral("WORK-CONCURRENT");
    concurrent_input.sensors.append({ .sensor_id = 1,
                                      .display_name = QStringLiteral("US1"),
                                      .measurement_status = QStringLiteral("CLEAR"),
                                      .distance_cm = 8,
                                      .updated_at = {} });
    auto concurrent_vision = vision;
    concurrent_vision.work_id = QStringLiteral("WORK-CONCURRENT");
    auto concurrent_gripper = gripper;
    concurrent_gripper.work_id = QStringLiteral("WORK-CONCURRENT");
    auto concurrent_sorting = sorting;
    concurrent_sorting.work_id = QStringLiteral("WORK-CONCURRENT");
    concurrent_sorting.destination = QStringLiteral("2");
    concurrent_sorting.sensors = {
        { .sensor_id = 1,
          .display_name = QStringLiteral("US2"),
          .measurement_status = QStringLiteral("CLEAR"),
          .distance_cm = 17,
          .updated_at = {} },
        { .sensor_id = 2,
          .display_name = QStringLiteral("US3"),
          .measurement_status = QStringLiteral("DETECTED"),
          .distance_cm = 11,
          .updated_at = {} },
        { .sensor_id = 3,
          .display_name = QStringLiteral("US4"),
          .measurement_status = QStringLiteral("CLEAR"),
          .distance_cm = 29,
          .updated_at = {} },
    };
    auto concurrent_line_tracer = line_tracer;
    concurrent_line_tracer.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    concurrent_line_tracer.current_state = QStringLiteral("DELIVERING");
    concurrent_line_tracer.work_id = QStringLiteral("WORK-CONCURRENT");
    concurrent_line_tracer.destination = QStringLiteral("2");

    logistics::control_center::FactoryTopViewWidget concurrent_view;
    concurrent_view.setProcesses(
        { concurrent_input, concurrent_vision, concurrent_gripper, concurrent_sorting, concurrent_line_tracer });
    assert(concurrent_view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#89d185")));
    assert(concurrent_view.nodeColor(QStringLiteral("vision")) == QColor(QStringLiteral("#75beff")));
    assert(concurrent_view.nodeColor(QStringLiteral("gripper")) == QColor(QStringLiteral("#75beff")));
    assert(concurrent_view.nodeColor(QStringLiteral("sorting")) == QColor(QStringLiteral("#75beff")));
    assert(concurrent_view.nodeColor(QStringLiteral("linetracer")) == QColor(QStringLiteral("#75beff")));
    assert(concurrent_view.nodeOpacity(QStringLiteral("input")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("vision")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("gripper")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("sorting")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("linetracer")) == 1.0);
    assert(concurrent_view.boxPosition(QStringLiteral("input")) == QPointF(143, 81));
    assert(concurrent_view.boxPosition(QStringLiteral("linetracer")) == concurrent_view.lineTracerPickupPosition(2));
    assert(concurrent_view.gripperAngle() == 90.0);
    assert(!concurrent_view.gripperProductVisible());
    assert(VisibleProducts(concurrent_view).size() == 1);
    assert(VisibleProducts(concurrent_view).front()->pos() == QPointF(504, 345));
    assert(concurrent_view.sortingServoAngle() == 45.0);
    assert(concurrent_view.sensorText(QStringLiteral("input"), 1) == QStringLiteral("8 cm"));
    assert(concurrent_view.sensorText(QStringLiteral("sorting"), 1) == QStringLiteral("17 cm"));
    assert(concurrent_view.sensorText(QStringLiteral("sorting"), 2) == QStringLiteral("11 cm"));
    assert(concurrent_view.sensorText(QStringLiteral("sorting"), 3) == QStringLiteral("29 cm"));

    concurrent_view.advanceAnimationsForTest();
    assert(concurrent_view.nodeOpacity(QStringLiteral("input")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("vision")) < 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("linetracer")) < 1.0);
    assert(concurrent_view.boxPosition(QStringLiteral("input")) == QPointF(242, 81));
    assert(concurrent_view.boxPosition(QStringLiteral("sorting")) == QPointF(504, 345));
    assert(concurrent_view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 345));
    assert(concurrent_view.lineArrowPositions().size() == 8);

    concurrent_vision.connection_state = logistics::contracts::mqtt::ConnectionState::kOffline;
    concurrent_view.setProcesses(
        { concurrent_input, concurrent_vision, concurrent_gripper, concurrent_sorting, concurrent_line_tracer });
    assert(concurrent_view.nodeColor(QStringLiteral("vision")) == QColor(QStringLiteral("#777777")));
    assert(concurrent_view.nodeOpacity(QStringLiteral("vision")) == 0.15);
    assert(concurrent_view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#89d185")));
    assert(concurrent_view.nodeColor(QStringLiteral("gripper")) == QColor(QStringLiteral("#75beff")));
    assert(concurrent_view.nodeColor(QStringLiteral("sorting")) == QColor(QStringLiteral("#75beff")));
    assert(concurrent_view.nodeColor(QStringLiteral("linetracer")) == QColor(QStringLiteral("#75beff")));
    assert(concurrent_view.nodeOpacity(QStringLiteral("input")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("gripper")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("sorting")) == 1.0);
    assert(concurrent_view.nodeOpacity(QStringLiteral("linetracer")) == 1.0);
    assert(concurrent_view.boxPosition(QStringLiteral("input")) == QPointF(242, 81));
    assert(concurrent_view.boxPosition(QStringLiteral("sorting")) == QPointF(504, 345));
    assert(concurrent_view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 345));
    assert(concurrent_view.gripperAngle() == 90.0);
    assert(!concurrent_view.gripperProductVisible());
    assert(VisibleProducts(concurrent_view).size() == 1);
    assert(concurrent_view.sortingServoAngle() == 45.0);

    auto emergency_input = concurrent_input;
    emergency_input.current_state = QStringLiteral("EMERGENCY_STOP");
    emergency_input.has_error = false;
    const auto emergency_visual = BuildFactoryNodeVisual(emergency_input);
    assert(emergency_visual.state == FactoryNodeVisualState::EmergencyStop);
    assert(emergency_visual.state != FactoryNodeVisualState::Error);
    concurrent_view.setProcesses(
        { emergency_input, concurrent_vision, concurrent_gripper, concurrent_sorting, concurrent_line_tracer });
    assert(concurrent_view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#ff3b30")));
    assert(concurrent_view.nodeOpacity(QStringLiteral("input")) == 1.0);
    const auto emergency_input_position = concurrent_view.boxPosition(QStringLiteral("input"));
    concurrent_view.advanceAnimationsForTest();
    assert(concurrent_view.nodeOpacity(QStringLiteral("input")) == 1.0);
    assert(concurrent_view.boxPosition(QStringLiteral("input")) == emergency_input_position);

    logistics::control_center::FactoryTopViewWidget view;
    view.resize(700, 500);
    view.setProcesses({ input, vision, gripper, sorting, line_tracer });
    const auto line_selection = view.nodeSelectionRect(QStringLiteral("linetracer"));
    assert(line_selection.left() <= view.lineTracerDestinationPosition(1).x());
    assert(line_selection.right() >= view.lineTracerPickupPosition(1).x());
    assert(line_selection.top() <= view.lineTracerPickupPosition(1).y());
    assert(line_selection.bottom() >= view.lineTracerDestinationPosition(3).y());
    assert(line_selection.top() > 200.0);
    assert(view.gripperPivotPosition() == QPointF(504, 81));
    assert(QLineF(view.gripperPivotPosition(), QPointF(440, 81)).length() == 64.0);
    assert(QLineF(view.gripperPivotPosition(), QPointF(504, 145)).length() == 64.0);
    view.show();
    application.processEvents();
    const auto scene_item_count = view.scene()->items().size();

    assert(view.sceneRect() == QRectF(0, 0, 700, 500));
    const QPointF sorting_drop_positions[]{ { 504, 250 }, { 504, 345 }, { 504, 442 } };
    for (int route = 1; route <= 3; ++route) {
        assert(view.lineTracerPickupPosition(route) == sorting_drop_positions[route - 1]);
        assert(view.lineTracerJunctionPosition(route).x() == view.lineTracerJunctionPosition(1).x());
    }
    assert(view.lineTracerJunctionPosition(1).y() < view.lineTracerJunctionPosition(3).y());
    assert(view.lineTracerPickupPosition(0).isNull());
    assert(view.lineTracerJunctionPosition(4).isNull());
    assert(view.lineTracerDestinationPosition(-1).isNull());
    const QLineF common_line(view.lineTracerJunctionPosition(1), view.lineTracerJunctionPosition(3));
    bool has_common_line = false;
    const QLineF input_line(QPointF(143, 81), QPointF(440, 81));
    bool has_input_line = false;
    for (auto* item : view.scene()->items()) {
        const auto* line = dynamic_cast<QGraphicsLineItem*>(item);
        if (line != nullptr && ((line->line().p1() == common_line.p1() && line->line().p2() == common_line.p2()) ||
                                (line->line().p1() == common_line.p2() && line->line().p2() == common_line.p1()))) {
            has_common_line = true;
        }
        if (line != nullptr && ((line->line().p1() == input_line.p1() && line->line().p2() == input_line.p2()) ||
                                (line->line().p1() == input_line.p2() && line->line().p2() == input_line.p1()))) {
            has_input_line = true;
        }
    }
    assert(has_common_line);
    assert(has_input_line);
    assert(view.nodeOpacity(QStringLiteral("input")) == 1.0);
    assert(view.nodeOpacity(QStringLiteral("linetracer")) == 0.15);
    assert(view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#89d185")));
    assert(view.nodeColor(QStringLiteral("linetracer")) == QColor(QStringLiteral("#777777")));
    assert(view.sensorText(QStringLiteral("sorting"), 2) == QStringLiteral("11 cm"));
    assert(view.gripperAngle() == 90.0);

    logistics::control_center::FactoryTopViewWidget routed_view;
    auto routed_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-ROUTED"),
                               QStringLiteral("PICKUP_READY_A"), QStringLiteral("WORK-A-TO-C"));
    routed_line.destination = QStringLiteral("3");
    routed_view.setProcesses({ routed_line });
    assert(routed_view.boxPosition(QStringLiteral("linetracer")) == routed_view.lineTracerPickupPosition(1));
    assert(routed_view.lineArrowPositions().isEmpty());

    routed_line.current_state = QStringLiteral("STOPPED");
    routed_view.setProcesses({ routed_line });
    const auto departure_position = routed_view.boxPosition(QStringLiteral("linetracer"));
    const auto arrows_before = routed_view.lineArrowPositions();
    assert(departure_position == routed_view.lineTracerPickupPosition(1));
    assert(arrows_before.size() == 8);
    routed_view.advanceAnimationsForTest();
    assert(routed_view.boxPosition(QStringLiteral("linetracer")) == departure_position);
    assert(routed_view.lineArrowPositions() == arrows_before);

    routed_line.current_state = QStringLiteral("FOLLOWING_LINE");
    routed_view.setProcesses({ routed_line });
    routed_view.advanceAnimationsForTest();
    assert(routed_view.lineArrowPositions() != arrows_before);

    routed_line.current_state = QStringLiteral("ARRIVED_C");
    routed_view.setProcesses({ routed_line });
    assert(routed_view.boxPosition(QStringLiteral("linetracer")) == routed_view.lineTracerDestinationPosition(3));
    assert(routed_view.lineArrowPositions().isEmpty());

    routed_line.current_state = QStringLiteral("PICKUP_READY_C");
    routed_line.work_id = QStringLiteral("WORK-C-TO-A");
    routed_line.destination = QStringLiteral("1");
    routed_view.setProcesses({ routed_line });
    assert(routed_view.boxPosition(QStringLiteral("linetracer")) == routed_view.lineTracerPickupPosition(3));
    routed_line.current_state = QStringLiteral("FOLLOWING_LINE");
    routed_view.setProcesses({ routed_line });
    assert(routed_view.boxPosition(QStringLiteral("linetracer")) == routed_view.lineTracerPickupPosition(3));
    assert(routed_view.lineArrowPositions().size() == 8);
    routed_line.current_state = QStringLiteral("UNLOADING_A");
    routed_view.setProcesses({ routed_line });
    assert(routed_view.boxPosition(QStringLiteral("linetracer")) == routed_view.lineTracerDestinationPosition(1));

    input.work_id = QStringLiteral("WORK-GRIPPER");
    vision.work_id = QStringLiteral("WORK-GRIPPER");
    sorting.work_id = QStringLiteral("WORK-GRIPPER");
    sorting.current_state = QStringLiteral("STOPPED");
    sorting.sensors.clear();

    view.setProcesses({ input });
    assert(VisibleProducts(view).size() == 1);

    view.setProcesses({ input, vision });
    assert(VisibleProducts(view).size() == 1);
    assert(VisibleProducts(view).front()->pos() == QPointF(440, 81));
    view.advanceAnimationsForTest();
    assert(VisibleProducts(view).front()->effectiveOpacity() < 1.0);

    gripper.current_state = QStringLiteral("PICKING");
    view.setProcesses({ input, vision, gripper, sorting });
    assert(VisibleProducts(view).size() == 1);
    assert(view.gripperProductVisible());
    assert(view.gripperProductPosition() == QPointF(440, 81));
    gripper.current_state = QStringLiteral("TRANSFERRING");
    view.setProcesses({ input, vision, gripper, sorting });
    assert(VisibleProducts(view).size() == 1);
    assert(view.gripperProductVisible());
    assert(view.gripperProductPosition() == QPointF(504, 145));
    gripper.current_state = QStringLiteral("PLACED");
    view.setProcesses({ input, gripper, sorting });
    assert(VisibleProducts(view).size() == 1);
    assert(view.gripperProductVisible());
    assert(view.gripperProductPosition() == QPointF(504, 250));

    sorting.current_state = QStringLiteral("SORTING");
    sorting.sensors = { { .sensor_id = 2,
                          .display_name = QStringLiteral("S2"),
                          .measurement_status = QStringLiteral("DETECTED"),
                          .distance_cm = 11,
                          .updated_at = {} } };
    view.setProcesses({ input, vision, gripper, sorting });
    assert(VisibleProducts(view).size() == 1);
    assert(VisibleProducts(view).front()->pos() == QPointF(504, 345));
    assert(!view.gripperProductVisible());

    gripper.work_id = QStringLiteral("WORK-MISMATCH");
    view.setProcesses({ input, gripper, sorting });
    assert(!view.gripperProductVisible());
    gripper.work_id = QStringLiteral("WORK-GRIPPER");
    gripper.connection_state = logistics::contracts::mqtt::ConnectionState::kOffline;
    view.setProcesses({ input, gripper, sorting });
    assert(!view.gripperProductVisible());
    gripper.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;

    auto other_input = input;
    other_input.work_id = QStringLiteral("WORK-INPUT-NEXT");
    view.setProcesses({ other_input, sorting });
    assert(VisibleProducts(view).size() == 2);

    line_tracer.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    line_tracer.current_state = QStringLiteral("FOLLOWING_LINE");
    line_tracer.work_id = QStringLiteral("WORK-ROUTE");
    sorting.work_id = QStringLiteral("WORK-ROUTE");
    const auto sorting_sensors = sorting.sensors;
    const struct {
        QString destination;
        qreal servo_angle;
        int sensor_id;
    } route_cases[] = {
        { QStringLiteral("1"), 0.0, 1 },
        { QStringLiteral("route-2"), 45.0, 2 },
        { QStringLiteral("destination-3"), 90.0, 3 },
    };
    for (const auto& route_case : route_cases) {
        sorting.destination = route_case.destination;
        sorting.sensors = { { .sensor_id = route_case.sensor_id,
                              .display_name = QStringLiteral("route sensor"),
                              .measurement_status = QStringLiteral("DETECTED"),
                              .distance_cm = 10,
                              .updated_at = {} } };
        line_tracer.destination = route_case.destination;
        view.setProcesses({ sorting, line_tracer });
        assert(view.sortingServoAngle() == route_case.servo_angle);
        assert(view.boxPosition(QStringLiteral("sorting")) == view.lineTracerPickupPosition(route_case.sensor_id));
    }
    sorting.sensors = sorting_sensors;
    sorting.destination.clear();
    line_tracer.destination.clear();
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
    assert(sorting_pinned == QPointF(504, 345));
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

    logistics::control_center::FactoryTopViewWidget position_view;
    auto positioned_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-POSITION"),
                                   QStringLiteral("STOPPED"), QStringLiteral("WORK-POSITION"));
    positioned_line.departure_position =
        LineTracerPositionStatus{ .area = QStringLiteral("DEPARTURE"), .location = QStringLiteral("A") };
    positioned_line.target_position =
        LineTracerPositionStatus{ .area = QStringLiteral("DESTINATION"), .location = QStringLiteral("C") };
    positioned_line.confirmed_position = positioned_line.departure_position;
    positioned_line.movement_state = QStringLiteral("MOVING");
    position_view.setProcesses({ positioned_line });
    assert(position_view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 250));
    const auto positioned_arrows = position_view.lineArrowPositions();
    position_view.advanceAnimationsForTest();
    assert(position_view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 250));
    assert(position_view.lineArrowPositions() == positioned_arrows);

    positioned_line.current_state = QStringLiteral("FOLLOWING_LINE");
    position_view.setProcesses({ positioned_line });
    position_view.advanceAnimationsForTest();
    assert(position_view.lineArrowPositions() != positioned_arrows);
    position_view.advanceAnimationsForTest();
    assert(position_view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 250));

    positioned_line.confirmed_position = positioned_line.target_position;
    positioned_line.movement_state = QStringLiteral("ARRIVED");
    position_view.setProcesses({ positioned_line });
    assert(position_view.boxPosition(QStringLiteral("linetracer")) == QPointF(80, 442));
    assert(position_view.lineArrowPositions().isEmpty());

    line_tracer.connection_state = logistics::contracts::mqtt::ConnectionState::kOnline;
    line_tracer.current_state = QStringLiteral("FOLLOWING_LINE");
    line_tracer.work_id = QStringLiteral("WORK-LT");
    line_tracer.destination = QStringLiteral("2");
    sorting.work_id = QStringLiteral("WORK-LT");
    sorting.destination = QStringLiteral("2");
    view.setProcesses({ sorting, line_tracer });
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 345));
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 345));
    line_tracer.current_state = QStringLiteral("COMPLETED");
    line_tracer.work_completed = true;
    view.setProcesses({ sorting, line_tracer });
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(80, 345));
    view.advanceAnimationsForTest();
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(80, 345));

    sorting.work_id = QStringLiteral("WORK-B");
    sorting.destination = QStringLiteral("3");
    line_tracer.work_id = QStringLiteral("WORK-A");
    line_tracer.destination = QStringLiteral("1");
    line_tracer.current_state = QStringLiteral("배송 완료");
    line_tracer.work_completed = false;
    visual = BuildFactoryNodeVisual(line_tracer);
    assert(visual.motion_phase != FactoryMotionPhase::LineCompleted);
    line_tracer.work_completed = true;
    visual = BuildFactoryNodeVisual(line_tracer);
    assert(visual.motion_phase == FactoryMotionPhase::LineCompleted);
    view.setProcesses({ sorting, line_tracer });
    assert(view.sortingServoAngle() == 90.0);
    assert(view.boxPosition(QStringLiteral("linetracer")) == QPointF(80, 250));

    line_tracer.work_id = QStringLiteral("WORK-C");
    line_tracer.destination.clear();
    line_tracer.current_state = QStringLiteral("FOLLOWING_LINE");
    line_tracer.work_completed = false;
    const auto completed_line_position = view.boxPosition(QStringLiteral("linetracer"));
    view.setProcesses({ sorting, line_tracer });
    view.advanceAnimationsForTest();
    assert(view.boxPosition(QStringLiteral("linetracer")) == completed_line_position);

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

    logistics::control_center::FactoryTopViewWidget safety_view;
    auto safety_input = Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-SAFE"), QStringLiteral("RUNNING"),
                                QStringLiteral("WORK-SAFE"));
    auto safety_sorting = Process(QStringLiteral("sorting"), QStringLiteral("PI-SORTING-SAFE"),
                                  QStringLiteral("SORTING"), QStringLiteral("WORK-SAFE"));
    safety_sorting.destination = QStringLiteral("2");
    auto safety_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-SAFE"),
                               QStringLiteral("FOLLOWING_LINE"), QStringLiteral("WORK-SAFE"));
    safety_line.destination = QStringLiteral("2");
    safety_view.setProcesses({ safety_input, safety_sorting, safety_line });
    safety_view.advanceAnimationsForTest();
    safety_input.has_error = true;
    safety_sorting.has_error = true;
    safety_line.has_error = true;
    safety_view.setProcesses({ safety_input, safety_sorting, safety_line });
    const auto error_input_position = safety_view.boxPosition(QStringLiteral("input"));
    const auto error_sorting_position = safety_view.boxPosition(QStringLiteral("sorting"));
    const auto error_line_position = safety_view.boxPosition(QStringLiteral("linetracer"));
    safety_view.advanceAnimationsForTest();
    safety_view.advanceAnimationsForTest();
    assert(safety_view.nodeColor(QStringLiteral("sorting")) == QColor(QStringLiteral("#f14c4c")));
    assert(safety_view.nodeOpacity(QStringLiteral("sorting")) == 1.0);
    assert(safety_view.boxPosition(QStringLiteral("input")) == error_input_position);
    assert(safety_view.boxPosition(QStringLiteral("sorting")) == error_sorting_position);
    assert(safety_view.boxPosition(QStringLiteral("linetracer")) == error_line_position);

    safety_input.has_error = false;
    safety_sorting.has_error = false;
    safety_line.has_error = false;
    safety_input.current_state = QStringLiteral("EMERGENCY_STOP");
    safety_sorting.current_state = QStringLiteral("EMERGENCY_STOP");
    safety_line.current_state = QStringLiteral("EMERGENCY_STOP");
    safety_view.setProcesses({ safety_input, safety_sorting, safety_line },
                             logistics::control_center::OverallProcessState::EmergencyStop);
    const auto stopped_input_position = safety_view.boxPosition(QStringLiteral("input"));
    const auto stopped_sorting_position = safety_view.boxPosition(QStringLiteral("sorting"));
    const auto stopped_line_position = safety_view.boxPosition(QStringLiteral("linetracer"));
    safety_view.advanceAnimationsForTest();
    safety_view.advanceAnimationsForTest();
    assert(safety_view.boxPosition(QStringLiteral("input")) == stopped_input_position);
    assert(safety_view.boxPosition(QStringLiteral("sorting")) == stopped_sorting_position);
    assert(safety_view.boxPosition(QStringLiteral("linetracer")) == stopped_line_position);
    safety_input.current_state = QStringLiteral("STOPPED");
    safety_view.setProcesses({ safety_input, safety_sorting, safety_line },
                             logistics::control_center::OverallProcessState::EmergencyStop);
    assert(safety_view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#cca700")));
    assert(safety_view.nodeColor(QStringLiteral("sorting")) == QColor(QStringLiteral("#ff3b30")));

    logistics::control_center::FactoryTopViewWidget sensor_stale_view;
    auto sensor_stale_input = Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-STALE"),
                                      QStringLiteral("RUNNING"), QStringLiteral("WORK-STALE"));
    auto sensor_stale_sorting = Process(QStringLiteral("sorting"), QStringLiteral("PI-SORTING-STALE"),
                                        QStringLiteral("SORTING"), QStringLiteral("WORK-STALE"));
    sensor_stale_sorting.destination = QStringLiteral("2");
    sensor_stale_view.setProcesses({ sensor_stale_input, sensor_stale_sorting });
    sensor_stale_view.advanceAnimationsForTest();
    const auto trusted_input_position = sensor_stale_view.boxPosition(QStringLiteral("input"));
    const auto trusted_sorting_position = sensor_stale_view.boxPosition(QStringLiteral("sorting"));
    sensor_stale_input.has_warning = true;
    sensor_stale_input.error_code = QStringLiteral("ERR-HEALTH-SENSOR-STALE");
    sensor_stale_input.sensors = { { .sensor_id = 1,
                                     .display_name = QStringLiteral("US1"),
                                     .measurement_status = QStringLiteral("DETECTED"),
                                     .distance_cm = 4,
                                     .updated_at = {} } };
    sensor_stale_sorting.has_warning = true;
    sensor_stale_sorting.error_code = QStringLiteral("ERR-HEALTH-SENSOR-STALE");
    sensor_stale_sorting.destination = QStringLiteral("3");
    sensor_stale_sorting.sensors = { { .sensor_id = 3,
                                       .display_name = QStringLiteral("US4"),
                                       .measurement_status = QStringLiteral("DETECTED"),
                                       .distance_cm = 6,
                                       .updated_at = {} } };
    sensor_stale_view.setProcesses({ sensor_stale_input, sensor_stale_sorting });
    assert(sensor_stale_view.nodeColor(QStringLiteral("input")) == QColor(QStringLiteral("#89d185")));
    assert(sensor_stale_view.nodeColor(QStringLiteral("sorting")) == QColor(QStringLiteral("#75beff")));
    assert(sensor_stale_view.sensorText(QStringLiteral("input"), 1) == QStringLiteral("4 cm"));
    assert(sensor_stale_view.sensorText(QStringLiteral("sorting"), 3) == QStringLiteral("6 cm"));
    assert(sensor_stale_view.boxPosition(QStringLiteral("input")) == trusted_input_position);
    assert(sensor_stale_view.boxPosition(QStringLiteral("sorting")) == trusted_sorting_position);
    sensor_stale_view.advanceAnimationsForTest();
    sensor_stale_view.advanceAnimationsForTest();
    assert(sensor_stale_view.boxPosition(QStringLiteral("input")) == trusted_input_position);
    assert(sensor_stale_view.boxPosition(QStringLiteral("sorting")) == trusted_sorting_position);

    logistics::control_center::FactoryTopViewWidget waiting_sensor_view;
    auto waiting_input =
        Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-WAITING"), QStringLiteral("STOPPED"), QString{});
    waiting_input.sensors.append({ .sensor_id = 1,
                                   .display_name = QStringLiteral("US1"),
                                   .measurement_status = QStringLiteral("CLEAR"),
                                   .distance_cm = 13,
                                   .updated_at = {} });
    waiting_sensor_view.setProcesses({ waiting_input });
    assert(waiting_sensor_view.sensorText(QStringLiteral("input"), 1) == QStringLiteral("13 cm"));
    bool waiting_sensor_visible = false;
    for (auto* item : waiting_sensor_view.scene()->items()) {
        const auto* label = dynamic_cast<QGraphicsSimpleTextItem*>(item);
        waiting_sensor_visible =
            waiting_sensor_visible ||
            (label != nullptr && label->text() == QStringLiteral("US1 13 cm") && label->isVisible());
    }
    assert(waiting_sensor_visible);

    const bool animation_preference = QApplication::isEffectEnabled(Qt::UI_AnimateCombo);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    logistics::control_center::FactoryTopViewWidget reduced_motion_view;
    auto reduced_input = Process(QStringLiteral("input"), QStringLiteral("PI-INPUT-REDUCED"), QStringLiteral("RUNNING"),
                                 QStringLiteral("WORK-REDUCED"));
    auto reduced_sorting = Process(QStringLiteral("sorting"), QStringLiteral("PI-SORTING-REDUCED"),
                                   QStringLiteral("SORTING"), QStringLiteral("WORK-REDUCED"));
    reduced_sorting.destination = QStringLiteral("3");
    auto reduced_line = Process(QStringLiteral("linetracer"), QStringLiteral("PI-LT-REDUCED"),
                                QStringLiteral("FOLLOWING_LINE"), QStringLiteral("WORK-REDUCED"));
    reduced_line.destination = QStringLiteral("3");
    reduced_motion_view.setProcesses({ reduced_input, vision, reduced_sorting, reduced_line });
    assert(reduced_motion_view.boxPosition(QStringLiteral("input")) == QPointF(341, 81));
    assert(reduced_motion_view.boxPosition(QStringLiteral("sorting")) == QPointF(504, 442));
    assert(reduced_motion_view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 442));
    const auto reduced_input_position = reduced_motion_view.boxPosition(QStringLiteral("input"));
    const auto reduced_sorting_position = reduced_motion_view.boxPosition(QStringLiteral("sorting"));
    const auto reduced_line_position = reduced_motion_view.boxPosition(QStringLiteral("linetracer"));
    reduced_motion_view.advanceAnimationsForTest();
    reduced_motion_view.advanceAnimationsForTest();
    assert(reduced_motion_view.nodeOpacity(QStringLiteral("vision")) == 1.0);
    assert(reduced_motion_view.boxPosition(QStringLiteral("input")) == reduced_input_position);
    assert(reduced_motion_view.boxPosition(QStringLiteral("sorting")) == reduced_sorting_position);
    assert(reduced_motion_view.boxPosition(QStringLiteral("linetracer")) == reduced_line_position);

    reduced_line.current_state = QStringLiteral("PICKUP_READY_C");
    reduced_motion_view.setProcesses({ reduced_line });
    reduced_line.current_state = QStringLiteral("FOLLOWING_LINE");
    reduced_motion_view.setProcesses({ reduced_line });
    assert(reduced_motion_view.boxPosition(QStringLiteral("linetracer")) == QPointF(504, 442));
    const auto reduced_return_position = reduced_motion_view.boxPosition(QStringLiteral("linetracer"));
    reduced_motion_view.advanceAnimationsForTest();
    reduced_motion_view.advanceAnimationsForTest();
    assert(reduced_motion_view.boxPosition(QStringLiteral("linetracer")) == reduced_return_position);

    reduced_input.sensors.append({ .sensor_id = 1,
                                   .display_name = QStringLiteral("US1"),
                                   .measurement_status = QStringLiteral("DETECTED"),
                                   .distance_cm = 4,
                                   .updated_at = {} });
    reduced_sorting.destination = QStringLiteral("1");
    reduced_motion_view.setProcesses({ reduced_input, reduced_sorting });
    assert(reduced_motion_view.boxPosition(QStringLiteral("input")) == QPointF(440, 81));
    assert(reduced_motion_view.boxPosition(QStringLiteral("sorting")) == QPointF(504, 250));
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, animation_preference);

    QString selected;
    QObject::connect(&view, &logistics::control_center::FactoryTopViewWidget::controlTargetSelected,
                     [&selected](const QString& id, const QString&) { selected = id; });
    view.selectProcessForTest(QStringLiteral("vision"));
    assert(selected == QStringLiteral("PI-VISION-01"));
    view.setSelectedDeviceId(QStringLiteral("PI-VISION-01"));
    assert(view.selectedDeviceId() == QStringLiteral("PI-VISION-01"));
    assert(view.focusPolicy() == Qt::StrongFocus);
    QGraphicsObject* vision_node = nullptr;
    for (auto* item : view.scene()->items()) {
        auto* object = item->toGraphicsObject();
        if (object != nullptr && object->property("processKey").toString() == QStringLiteral("vision")) {
            vision_node = object;
            break;
        }
    }
    assert(vision_node != nullptr);
    selected.clear();
    QKeyEvent select_with_keyboard(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    assert(view.scene()->sendEvent(vision_node, &select_with_keyboard));
    assert(selected == QStringLiteral("PI-VISION-01"));
    assert(view.scene()->items().size() == scene_item_count);
}
