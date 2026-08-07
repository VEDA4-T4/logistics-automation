#include "logistics/control_center/factory_top_view.hpp"

#include <QApplication>
#include <QBrush>
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsObject>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QHash>
#include <QLineF>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QTimer>
#include <functional>

namespace logistics::control_center {
namespace {

QString NormalizedState(const QString& current_state) {
    return current_state.trimmed().toUpper();
}

std::optional<int> RouteSuffix(const QString& state, QStringView prefix) {
    if (!state.startsWith(prefix) || state.size() != prefix.size() + 1) {
        return std::nullopt;
    }
    const auto suffix = state.back();
    return suffix >= QLatin1Char('A') && suffix <= QLatin1Char('C') ? std::optional<int>(suffix.unicode() - 'A' + 1)
                                                                    : std::nullopt;
}

bool HasDestinationSuffix(const QString& state, QStringView prefix) {
    if (!state.startsWith(prefix) || state.size() != prefix.size() + 1) {
        return false;
    }
    const auto suffix = state.back();
    return suffix >= QLatin1Char('1') && suffix <= QLatin1Char('3');
}

FactoryMotionPhase MotionPhaseFor(const ProcessUnitStatus& process, const QString& state) {
    if (process.work_completed && process.key == QString::fromLatin1(kLineTracerProcessKey)) {
        return FactoryMotionPhase::LineCompleted;
    }
    if (process.key == QString::fromLatin1(kVisionProcessKey) &&
        (state == QStringLiteral("VISION_PROCESSING") || state == QStringLiteral("WORK_ASSIGNED") ||
         state == QStringLiteral("AWAITING_WORK_ID"))) {
        return FactoryMotionPhase::VisionProcessing;
    }
    if (process.key == QString::fromLatin1(kGripperProcessKey)) {
        if (state == QStringLiteral("PICKING")) {
            return FactoryMotionPhase::GripperPick;
        }
        if (state == QStringLiteral("TRANSFERRING")) {
            return FactoryMotionPhase::GripperTransfer;
        }
        if (state == QStringLiteral("PLACED") || state == QStringLiteral("COMPLETED")) {
            return FactoryMotionPhase::GripperPlaced;
        }
    }
    if (process.key == QString::fromLatin1(kSortingProcessKey) &&
        (state == QStringLiteral("SORTING") || state == QStringLiteral("ROUTING"))) {
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
    if (process.key == QString::fromLatin1(kLineTracerProcessKey) &&
        (state == QStringLiteral("DELIVERING") || state == QStringLiteral("FOLLOWING_LINE"))) {
        return FactoryMotionPhase::LineFollowing;
    }
    return FactoryMotionPhase::InputConveyor;
}

bool IsWorkingState(const ProcessUnitStatus& process, const QString& state) {
    if (process.key == QString::fromLatin1(kVisionProcessKey)) {
        return state == QStringLiteral("WORK_ASSIGNED") || state == QStringLiteral("AWAITING_WORK_ID") ||
               state == QStringLiteral("VISION_PROCESSING");
    }
    if (process.key == QString::fromLatin1(kInputProcessKey)) {
        return state == QStringLiteral("BUSY");
    }
    if (process.key == QString::fromLatin1(kGripperProcessKey)) {
        return state == QStringLiteral("PICKING") || state == QStringLiteral("TRANSFERRING") ||
               state == QStringLiteral("PLACED") || state == QStringLiteral("COMPLETED");
    }
    if (process.key == QString::fromLatin1(kSortingProcessKey)) {
        return state == QStringLiteral("BUSY") || state == QStringLiteral("SORTING") ||
               state == QStringLiteral("ROUTING") || state == QStringLiteral("RETURNING_HOME") ||
               HasDestinationSuffix(state, u"GATE_MOVING_DEST_") || HasDestinationSuffix(state, u"WAITING_ITEM_DEST_");
    }
    if (process.key == QString::fromLatin1(kLineTracerProcessKey)) {
        return state == QStringLiteral("DELIVERING") || state == QStringLiteral("FOLLOWING_LINE") ||
               state == QStringLiteral("CORRECTING") || RouteSuffix(state, u"PICKUP_READY_").has_value() ||
               RouteSuffix(state, u"ARRIVED_").has_value() || RouteSuffix(state, u"UNLOADING_").has_value() ||
               RouteSuffix(state, u"LOAD_ON_").has_value() || RouteSuffix(state, u"LOAD_OFF_").has_value();
    }
    return false;
}

bool IsRunningState(const ProcessUnitStatus& process, const QString& state) {
    return state == QStringLiteral("RUNNING") || state == QStringLiteral("ONLINE") ||
           (process.key == QString::fromLatin1(kVisionProcessKey) && state == QStringLiteral("WAITING_FOR_PRODUCT")) ||
           (process.key == QString::fromLatin1(kLineTracerProcessKey) && RouteSuffix(state, u"PARKED_").has_value());
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
    visual.motion_enabled = !process.work_id.isEmpty();
    return visual;
}

FactoryNodeVisual RunningVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::Running;
    visual.opacity = 1.0;
    return visual;
}

FactoryNodeVisual WaitingVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.opacity = 1.0;
    return visual;
}

}  // namespace

QString FactoryDistanceText(int distance_cm) {
    return distance_cm >= 0 ? QStringLiteral("%1 cm").arg(distance_cm) : QStringLiteral("-- cm");
}

std::optional<int> FactoryRouteIndex(const QString& current_state) {
    return DestinationRouteIndex(current_state);
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
    if ((process.work_completed && process.key == QString::fromLatin1(kLineTracerProcessKey)) ||
        IsWorkingState(process, state)) {
        return WorkingVisual(process, state);
    }
    if (IsRunningState(process, state)) {
        return RunningVisual(process);
    }
    return WaitingVisual(process);
}

namespace {

constexpr QRectF kFactoryScene{ 0, 0, 700, 500 };
constexpr QPointF kInputPositions[]{ { 143, 81 }, { 242, 81 }, { 341, 81 }, { 440, 81 } };
constexpr QPointF kGripperPivot{ 440, 145 };
constexpr qreal kGripperReach = 64.0;
constexpr QPointF kSortingFeed{ 504, 145 };
constexpr QPointF kSortingPositions[]{ { 504, 250 }, { 504, 345 }, { 504, 442 } };
constexpr QPointF kLineIntersections[]{ { 292, 250 }, { 292, 345 }, { 292, 442 } };
constexpr QPointF kLineDestinations[]{ { 80, 250 }, { 80, 345 }, { 80, 442 } };

std::optional<int> LineTracerPositionRoute(const LineTracerPositionStatus& position) {
    const auto location = position.location.trimmed().toUpper();
    return location.size() == 1 && location.front() >= QLatin1Char('A') && location.front() <= QLatin1Char('C')
               ? std::optional<int>{ location.front().unicode() - 'A' }
               : std::nullopt;
}

std::optional<QPointF> LineTracerPositionPoint(const LineTracerPositionStatus& position) {
    const auto route = LineTracerPositionRoute(position);
    const auto area = position.area.trimmed().toUpper();
    if (!route.has_value()) {
        return std::nullopt;
    }
    if (area == QStringLiteral("DEPARTURE")) {
        return kLineDestinations[*route];
    }
    if (area == QStringLiteral("DESTINATION")) {
        return kSortingPositions[*route];
    }
    return std::nullopt;
}

QList<QPointF> LineTracerPositionPath(const LineTracerPositionStatus& departure,
                                      const LineTracerPositionStatus& target) {
    const auto departure_route = LineTracerPositionRoute(departure);
    const auto target_route = LineTracerPositionRoute(target);
    const auto departure_point = LineTracerPositionPoint(departure);
    const auto target_point = LineTracerPositionPoint(target);
    if (!departure_route.has_value() || !target_route.has_value() || !departure_point.has_value() ||
        !target_point.has_value()) {
        return {};
    }
    if (*departure_point == *target_point) {
        return { *departure_point };
    }

    QList<QPointF> path{ *departure_point };
    const int step = *departure_route <= *target_route ? 1 : -1;
    for (int route = *departure_route;; route += step) {
        path.append(kLineIntersections[route]);
        if (route == *target_route) {
            break;
        }
    }
    path.append(*target_point);
    return path;
}

class ProcessGraphicsGroup final : public QGraphicsObject {
public:
    ProcessGraphicsGroup(QString process_key, std::function<void(const QString&)> selected)
        : process_key_(std::move(process_key)), selected_(std::move(selected)) {
        setAcceptedMouseButtons(Qt::LeftButton);
        setCursor(Qt::PointingHandCursor);
    }

    [[nodiscard]] QRectF boundingRect() const override {
        return childrenBoundingRect().adjusted(-4, -4, 4, 4);
    }

    void paint(QPainter*, const QStyleOptionGraphicsItem*, QWidget*) override {}

protected:
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            selected_(process_key_);
            event->accept();
            return;
        }
        QGraphicsObject::mouseReleaseEvent(event);
    }

private:
    QString process_key_;
    std::function<void(const QString&)> selected_;
};

QColor ColorFor(FactoryNodeVisualState state) {
    switch (state) {
        case FactoryNodeVisualState::Disconnected:
            return QColor(QStringLiteral("#777777"));
        case FactoryNodeVisualState::EmergencyStop:
        case FactoryNodeVisualState::Error:
            return QColor(QStringLiteral("#f14c4c"));
        case FactoryNodeVisualState::Working:
            return QColor(QStringLiteral("#75beff"));
        case FactoryNodeVisualState::Running:
            return QColor(QStringLiteral("#89d185"));
        case FactoryNodeVisualState::Waiting:
            return QColor(QStringLiteral("#ffffff"));
    }
    return QColor(QStringLiteral("#9d9d9d"));
}

bool AllowsMotion(const QString& process_key, const FactoryNodeVisual& visual) {
    if (visual.motion_enabled) {
        return true;
    }
    return visual.state == FactoryNodeVisualState::Running && !visual.work_id.isEmpty() &&
           (process_key == QString::fromLatin1(kInputProcessKey) ||
            process_key == QString::fromLatin1(kSortingProcessKey));
}

bool HasLiveTelemetry(const FactoryNodeVisual& visual) {
    return visual.state != FactoryNodeVisualState::Disconnected && visual.state != FactoryNodeVisualState::Error &&
           visual.state != FactoryNodeVisualState::EmergencyStop;
}

bool HasStaleSensorWarning(const ProcessUnitStatus& process) {
    return process.has_warning && IsSensorStaleErrorCode(process.error_code);
}

std::optional<int> DetectedSensor(const FactoryNodeVisual& visual) {
    for (const auto& sensor : visual.sensors) {
        if (sensor.detected) {
            return sensor.sensor_id;
        }
    }
    return std::nullopt;
}

}  // namespace

struct FactoryTopViewWidget::Impl {
    enum class LineTravelLeg { Idle, Outbound, Returning };

    struct NodeItems {
        ProcessGraphicsGroup* group{ nullptr };
        QGraphicsEllipseItem* state_marker{ nullptr };
        QGraphicsRectItem* selection_outline{ nullptr };
        QGraphicsItem* moving_item{ nullptr };
        QList<QGraphicsLineItem*> state_lines;
        QList<QAbstractGraphicsShapeItem*> state_shapes;
        QHash<int, QGraphicsSimpleTextItem*> sensor_labels;
        QHash<int, QString> sensor_text;
        FactoryNodeVisual visual;
        QString display_name;
        QColor color{ QStringLiteral("#9d9d9d") };
        bool motion_enabled{ false };
        int animation_phase{ 0 };
        int pulse_phase{ 0 };
    };

    explicit Impl(FactoryTopViewWidget* owner) : owner(owner), scene(new QGraphicsScene(owner)) {
        owner->setScene(scene);
        owner->setSceneRect(kFactoryScene);
        owner->setBackgroundBrush(QColor(QStringLiteral("#181818")));
        owner->setFrameShape(QFrame::NoFrame);
        owner->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        owner->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        owner->setRenderHint(QPainter::Antialiasing);
        buildScene();
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { tick(); });
        timer.start(160);
    }

    NodeItems& addNode(const QString& key, const QString& display_name, const QPointF& label_position) {
        NodeItems node;
        node.group =
            new ProcessGraphicsGroup(key, [this](const QString& selected_key) { owner->selectProcess(selected_key); });
        node.display_name = display_name;
        node.state_marker =
            new QGraphicsEllipseItem(QRectF(label_position.x(), label_position.y(), 10, 10), node.group);
        auto* label = new QGraphicsSimpleTextItem(display_name, node.group);
        label->setBrush(QColor(QStringLiteral("#cccccc")));
        label->setPos(label_position + QPointF(15, -4));
        nodes.insert(key, node);
        return nodes[key];
    }

    static QGraphicsLineItem* addStateLine(NodeItems& node, const QLineF& line, qreal width = 4.0) {
        auto* item = new QGraphicsLineItem(line, node.group);
        item->setPen(QPen(node.color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        node.state_lines.append(item);
        return item;
    }

    static QGraphicsRectItem* addBox(NodeItems& node, const QPointF& position) {
        auto* box = new QGraphicsRectItem(QRectF(-8, -8, 16, 16), node.group);
        box->setBrush(QColor(QStringLiteral("#ce9178")));
        box->setPen(QPen(QColor(QStringLiteral("#f0f0f0")), 1));
        box->setPos(position);
        node.moving_item = box;
        return box;
    }

    static void finalizeNode(NodeItems& node) {
        for (auto* child : node.group->childItems()) {
            child->setAcceptedMouseButtons(Qt::NoButton);
        }
        node.selection_outline =
            new QGraphicsRectItem(node.group->childrenBoundingRect().adjusted(-5, -5, 5, 5), node.group);
        QPen selection_pen(QColor(QStringLiteral("#4daafc")), 2);
        selection_pen.setCosmetic(true);
        node.selection_outline->setPen(selection_pen);
        node.selection_outline->setBrush(Qt::NoBrush);
        node.selection_outline->setZValue(100);
        node.selection_outline->setAcceptedMouseButtons(Qt::NoButton);
        node.selection_outline->hide();
        node.group->scene()->update(node.group->boundingRect());
    }

    void buildScene() {
        auto& input = addNode(QString::fromLatin1(kInputProcessKey), QStringLiteral("Input"), QPointF(32, 35));
        addStateLine(input, QLineF(kInputPositions[0], kInputPositions[3]), 6);
        addBox(input, kInputPositions[0]);
        auto* input_sensor = new QGraphicsSimpleTextItem(QStringLiteral("US1 -- cm"), input.group);
        input_sensor->setBrush(QColor(QStringLiteral("#cccccc")));
        input_sensor->setPos(356, 92);
        input.sensor_labels.insert(1, input_sensor);
        scene->addItem(input.group);
        finalizeNode(input);

        auto& vision = addNode(QString::fromLatin1(kVisionProcessKey), QStringLiteral("Vision"), QPointF(335, 35));
        auto* camera =
            new QGraphicsRectItem(QRectF(kInputPositions[3] - QPointF(15, 15), QSizeF(30, 30)), vision.group);
        camera->setBrush(QColor(QStringLiteral("#2d2d30")));
        camera->setPen(QPen(vision.color, 3));
        vision.state_shapes.append(camera);
        addStateLine(vision, QLineF(kInputPositions[3] - QPointF(9, 7), kInputPositions[3] + QPointF(9, 7)), 3);
        addBox(vision, kInputPositions[3]);
        scene->addItem(vision.group);
        finalizeNode(vision);

        auto& gripper = addNode(QString::fromLatin1(kGripperProcessKey), QStringLiteral("Gripper"), QPointF(405, 105));
        auto* pivot =
            new QGraphicsEllipseItem(QRectF(kGripperPivot.x() - 8, kGripperPivot.y() - 8, 16, 16), gripper.group);
        pivot->setBrush(QColor(QStringLiteral("#2d2d30")));
        pivot->setPen(QPen(gripper.color, 3));
        gripper.state_shapes.append(pivot);
        gripper_arm = addStateLine(gripper, QLineF(kGripperPivot, kGripperPivot - QPointF(0, kGripperReach)), 7);
        gripper_jaw_left = addStateLine(gripper, QLineF(432, 81, 440, 81), 3);
        gripper_jaw_right = addStateLine(gripper, QLineF(440, 81, 448, 81), 3);
        gripper_product = new QGraphicsRectItem(QRectF(-8, -8, 16, 16), gripper.group);
        gripper_product->setBrush(QColor(QStringLiteral("#ce9178")));
        gripper_product->setPen(QPen(QColor(QStringLiteral("#f0f0f0")), 1));
        gripper_product->setPos(kInputPositions[3]);
        gripper_product->hide();
        scene->addItem(gripper.group);
        finalizeNode(gripper);

        auto& sorting = addNode(QString::fromLatin1(kSortingProcessKey), QStringLiteral("Sorting"), QPointF(530, 85));
        addStateLine(sorting, QLineF(kSortingFeed, kSortingPositions[2]), 6);
        addBox(sorting, kSortingPositions[0]);
        for (int sensor_id = 1; sensor_id <= 3; ++sensor_id) {
            auto* sensor = new QGraphicsSimpleTextItem(QStringLiteral("US%1 -- cm").arg(sensor_id + 1), sorting.group);
            sensor->setBrush(QColor(QStringLiteral("#cccccc")));
            sensor->setPos(kSortingPositions[sensor_id - 1] + QPointF(18, -10));
            sorting.sensor_labels.insert(sensor_id, sensor);
        }
        sorting_servo = addStateLine(sorting, QLineF(485, 297, 505, 297), 5);
        scene->addItem(sorting.group);
        finalizeNode(sorting);

        auto& line_tracer =
            addNode(QString::fromLatin1(kLineTracerProcessKey), QStringLiteral("Line tracer"), QPointF(525, 280));
        for (int route = 0; route < 3; ++route) {
            addStateLine(line_tracer, QLineF(kLineIntersections[route], kLineDestinations[route]), 3);
            addStateLine(line_tracer, QLineF(kLineIntersections[route], kSortingPositions[route]), 3);
        }
        addStateLine(line_tracer, QLineF(kLineIntersections[0], kLineIntersections[2]), 3);
        auto* tracer = new QGraphicsEllipseItem(QRectF(-7, -7, 14, 14), line_tracer.group);
        tracer->setBrush(QColor(QStringLiteral("#dcdcaa")));
        tracer->setPen(QPen(QColor(QStringLiteral("#f0f0f0")), 1));
        tracer->setPos(kSortingPositions[0]);
        line_tracer.moving_item = tracer;
        scene->addItem(line_tracer.group);
        finalizeNode(line_tracer);

        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) {
            applyVisual(iterator.value());
        }
    }

    void applyVisual(NodeItems& node) {
        node.color = ColorFor(node.visual.state);
        node.group->setOpacity(node.visual.opacity);
        node.state_marker->setPen(QPen(node.color, 1));
        node.state_marker->setBrush(node.color);
        for (auto* line : node.state_lines) {
            auto pen = line->pen();
            pen.setColor(node.color);
            line->setPen(pen);
        }
        for (auto* shape : node.state_shapes) {
            auto pen = shape->pen();
            pen.setColor(node.color);
            shape->setPen(pen);
        }
    }

    void updateSensors(NodeItems& node) {
        node.sensor_text.clear();
        for (auto iterator = node.sensor_labels.begin(); iterator != node.sensor_labels.end(); ++iterator) {
            node.sensor_text.insert(iterator.key(), QStringLiteral("-- cm"));
            iterator.value()->setText(
                QStringLiteral("US%1 -- cm").arg(iterator.key() + (node.sensor_labels.size() == 1 ? 0 : 1)));
        }
        if (node.visual.state == FactoryNodeVisualState::Disconnected) {
            return;
        }
        for (const auto& sensor : node.visual.sensors) {
            node.sensor_text.insert(sensor.sensor_id, sensor.distance_text);
            if (auto* label = node.sensor_labels.value(sensor.sensor_id, nullptr); label != nullptr) {
                const auto number = sensor.sensor_id + (node.sensor_labels.size() == 1 ? 0 : 1);
                label->setText(QStringLiteral("US%1 %2").arg(number).arg(sensor.distance_text));
            }
        }
    }

    void applyGripperPhase(FactoryMotionPhase phase) {
        if (phase != FactoryMotionPhase::GripperPick && phase != FactoryMotionPhase::GripperTransfer &&
            phase != FactoryMotionPhase::GripperPlaced) {
            return;
        }
        gripper_angle = phase == FactoryMotionPhase::GripperPick ? 0.0 : 90.0;
        const bool open = phase == FactoryMotionPhase::GripperPlaced;
        const QPointF end = gripper_angle == 0.0 ? kGripperPivot - QPointF(0, kGripperReach)
                                                 : kGripperPivot + QPointF(kGripperReach, 0);
        gripper_arm->setLine(QLineF(kGripperPivot, end));
        if (gripper_angle == 0.0) {
            const qreal spread = open ? 8.0 : 3.0;
            gripper_jaw_left->setLine(QLineF(end.x() - spread, end.y(), end.x(), end.y()));
            gripper_jaw_right->setLine(QLineF(end.x(), end.y(), end.x() + spread, end.y()));
        } else {
            const qreal spread = open ? 8.0 : 3.0;
            gripper_jaw_left->setLine(QLineF(end.x(), end.y() - spread, end.x(), end.y()));
            gripper_jaw_right->setLine(QLineF(end.x(), end.y(), end.x(), end.y() + spread));
        }
    }

    void applyGripperProduct(FactoryMotionPhase phase, bool visible) {
        gripper_product->setVisible(visible);
        if (!visible) {
            return;
        }
        if (phase == FactoryMotionPhase::GripperPick) {
            gripper_product->setPos(kInputPositions[3]);
        } else if (phase == FactoryMotionPhase::GripperTransfer) {
            gripper_product->setPos(kSortingFeed);
        } else {
            gripper_product->setPos(kSortingPositions[0]);
        }
    }

    void applySortingRoute(const ProcessUnitStatus& process) {
        const auto route = FactoryRouteIndex(process.destination);
        if (!route.has_value()) {
            return;
        }
        sorting_servo_angle = (*route - 1) * 45.0;
        QLineF servo(QPointF(485, 297), QPointF(505, 297));
        servo.setAngle(-sorting_servo_angle);
        sorting_servo->setLine(servo);
    }

    void snapToFinalGeometry(const QString& process_key, NodeItems& node) {
        if (!node.motion_enabled) {
            return;
        }
        if (process_key == QString::fromLatin1(kInputProcessKey)) {
            node.moving_item->setPos(DetectedSensor(node.visual).has_value() ? kInputPositions[3] : kInputPositions[2]);
        } else if (process_key == QString::fromLatin1(kSortingProcessKey)) {
            const auto sensor = DetectedSensor(node.visual);
            node.moving_item->setPos(sensor.has_value() && *sensor >= 1 && *sensor <= 3 ? kSortingPositions[*sensor - 1]
                                     : sorting_target_route.has_value() ? kSortingPositions[*sorting_target_route - 1]
                                                                        : kSortingPositions[2]);
        } else if (process_key == QString::fromLatin1(kLineTracerProcessKey) && !line_path.isEmpty()) {
            line_path_index = line_path.size() - 1;
            node.moving_item->setPos(line_path.back());
        }
    }

    void beginLineTravel(NodeItems& node, const QString& work_id, int target_route, LineTravelLeg leg) {
        const int origin_route = line_origin_route.value_or(target_route);
        line_work_id = work_id;
        line_target_route = target_route;
        line_travel_leg = leg;
        line_path.clear();
        line_path_index = 0;

        if (leg == LineTravelLeg::Returning) {
            line_path = { kSortingPositions[target_route - 1], kLineIntersections[target_route - 1],
                          kLineDestinations[target_route - 1] };
        } else {
            line_path.append(kLineDestinations[origin_route - 1]);
            const int step = origin_route <= target_route ? 1 : -1;
            for (int route = origin_route;; route += step) {
                line_path.append(kLineIntersections[route - 1]);
                if (route == target_route) {
                    break;
                }
            }
            line_path.append(kSortingPositions[target_route - 1]);
        }
        node.moving_item->setPos(line_path.front());
    }

    void setProcesses(const QList<ProcessUnitStatus>& processes, OverallProcessState overall_state) {
        const auto visual_for = [overall_state](const ProcessUnitStatus& process) {
            auto visual = BuildFactoryNodeVisual(process);
            if (overall_state == OverallProcessState::EmergencyStop &&
                visual.state != FactoryNodeVisualState::Disconnected) {
                visual = EmergencyVisual(process);
            }
            return visual;
        };
        QString input_work_id;
        QString vision_work_id;
        QString sorting_work_id;
        for (const auto& process : processes) {
            const auto visual = visual_for(process);
            if (!HasLiveTelemetry(visual) || process.work_id.isEmpty()) {
                continue;
            }
            if (process.key == QString::fromLatin1(kInputProcessKey)) {
                input_work_id = process.work_id;
            } else if (process.key == QString::fromLatin1(kVisionProcessKey)) {
                vision_work_id = process.work_id;
            } else if (process.key == QString::fromLatin1(kSortingProcessKey)) {
                sorting_work_id = process.work_id;
            }
        }

        for (const auto& process : processes) {
            auto iterator = nodes.find(process.key);
            if (iterator == nodes.end()) {
                continue;
            }
            auto& node = iterator.value();
            const auto previous_sensor = DetectedSensor(node.visual);
            node.visual = visual_for(process);
            node.motion_enabled = AllowsMotion(process.key, node.visual);
            const bool stale_motion_telemetry = (process.key == QString::fromLatin1(kInputProcessKey) ||
                                                 process.key == QString::fromLatin1(kSortingProcessKey)) &&
                                                HasStaleSensorWarning(process);
            node.motion_enabled = node.motion_enabled && !stale_motion_telemetry;
            if (process.key == QString::fromLatin1(kLineTracerProcessKey) &&
                node.visual.motion_phase == FactoryMotionPhase::LineCompleted) {
                node.motion_enabled = false;
            }
            if (!process.display_name.isEmpty()) {
                node.display_name = process.display_name;
            }
            updateSensors(node);
            applyVisual(node);
            const bool telemetry_live = HasLiveTelemetry(node.visual);
            const bool motion_telemetry_live = telemetry_live && !stale_motion_telemetry;

            if (process.key == QString::fromLatin1(kInputProcessKey)) {
                if (motion_telemetry_live && DetectedSensor(node.visual).has_value()) {
                    node.moving_item->setPos(kInputPositions[3]);
                }
            } else if (process.key == QString::fromLatin1(kGripperProcessKey)) {
                applyGripperPhase(node.visual.motion_phase);
                const bool input_matches = !input_work_id.isEmpty() && input_work_id == process.work_id;
                const bool sorting_matches = !sorting_work_id.isEmpty() && sorting_work_id == process.work_id;
                const bool placed = node.visual.motion_phase == FactoryMotionPhase::GripperPlaced;
                const bool trusted_work = placed && !sorting_work_id.isEmpty() ? sorting_matches : input_matches;
                const bool product_phase = node.visual.motion_phase == FactoryMotionPhase::GripperPick ||
                                           node.visual.motion_phase == FactoryMotionPhase::GripperTransfer || placed;
                applyGripperProduct(node.visual.motion_phase, telemetry_live && product_phase && trusted_work);
            } else if (process.key == QString::fromLatin1(kSortingProcessKey)) {
                if (motion_telemetry_live) {
                    sorting_target_route = FactoryRouteIndex(process.destination);
                    if (const auto sensor = DetectedSensor(node.visual);
                        sensor.has_value() && *sensor >= 1 && *sensor <= 3) {
                        node.moving_item->setPos(kSortingPositions[*sensor - 1]);
                    } else if (previous_sensor.has_value() && *previous_sensor >= 1 && *previous_sensor <= 3) {
                        node.animation_phase = *previous_sensor - 1;
                    }
                    applySortingRoute(process);
                }
            } else if (process.key == QString::fromLatin1(kLineTracerProcessKey)) {
                if (!telemetry_live) {
                    node.motion_enabled = false;
                    continue;
                }

                if (process.confirmed_position.has_value()) {
                    node.moving_item->show();
                    const auto confirmed_point = LineTracerPositionPoint(*process.confirmed_position);
                    if (confirmed_point.has_value() && process.movement_state == QStringLiteral("MOVING") &&
                        process.departure_position.has_value() && process.target_position.has_value()) {
                        const auto reported_path =
                            LineTracerPositionPath(*process.departure_position, *process.target_position);
                        const bool path_changed = reported_path.isEmpty() || line_path.isEmpty() ||
                                                  line_path.front() != reported_path.front() ||
                                                  line_path.back() != reported_path.back() ||
                                                  line_work_id != process.work_id;
                        if (!reported_path.isEmpty() && path_changed) {
                            line_path = reported_path;
                            line_path_index = 0;
                            line_work_id = process.work_id;
                            node.moving_item->setPos(*confirmed_point);
                        }
                        node.motion_enabled = line_path.size() > 1;
                        if (!QApplication::isEffectEnabled(Qt::UI_AnimateCombo) && !line_path.isEmpty()) {
                            line_path_index = line_path.size() - 1;
                            node.moving_item->setPos(line_path.back());
                        }
                    } else if (confirmed_point.has_value()) {
                        line_travel_leg = LineTravelLeg::Idle;
                        line_path.clear();
                        line_path_index = 0;
                        line_work_id = process.work_id;
                        node.motion_enabled = false;
                        node.moving_item->setPos(*confirmed_point);
                    }
                    continue;
                }
                if (NormalizedState(process.current_state) == QStringLiteral("POSITION_UNKNOWN")) {
                    node.motion_enabled = false;
                    node.moving_item->hide();
                    continue;
                }
                node.moving_item->show();

                const auto state = NormalizedState(process.current_state);
                const auto arrived_route = RouteSuffix(state, u"ARRIVED_");
                const auto unloading_route = RouteSuffix(state, u"UNLOADING_");
                const auto pickup_route = RouteSuffix(state, u"PICKUP_READY_");
                if (const auto route = arrived_route.has_value() ? arrived_route : unloading_route; route.has_value()) {
                    line_origin_route = route;
                    line_target_route = route;
                    line_travel_leg = LineTravelLeg::Idle;
                    line_path.clear();
                    line_path_index = 0;
                    line_work_id = process.work_id;
                    node.motion_enabled = false;
                    node.moving_item->setPos(kLineDestinations[*route - 1]);
                } else if (pickup_route.has_value()) {
                    const auto reported_target = FactoryRouteIndex(process.destination);
                    const bool matching_work =
                        !process.work_id.isEmpty() && (line_work_id.isEmpty() || line_work_id == process.work_id);
                    if (matching_work && reported_target == pickup_route &&
                        (!line_target_route.has_value() || line_target_route == pickup_route)) {
                        beginLineTravel(node, process.work_id, *pickup_route, LineTravelLeg::Returning);
                    }
                    node.motion_enabled = false;
                } else if (node.visual.motion_phase == FactoryMotionPhase::LineCompleted) {
                    if (const auto route = FactoryRouteIndex(process.destination); route.has_value()) {
                        line_origin_route = route;
                        line_target_route = route;
                        line_travel_leg = LineTravelLeg::Idle;
                        line_path.clear();
                        line_path_index = 0;
                        line_work_id = process.work_id;
                        node.moving_item->setPos(kLineDestinations[*route - 1]);
                    }
                    node.motion_enabled = false;
                } else if (node.visual.motion_phase == FactoryMotionPhase::LineFollowing) {
                    const auto target_route = FactoryRouteIndex(process.destination);
                    if (!target_route.has_value()) {
                        node.motion_enabled = false;
                        line_target_route.reset();
                        line_path.clear();
                    } else if (line_work_id != process.work_id || line_target_route != target_route ||
                               line_travel_leg == LineTravelLeg::Idle || line_path.isEmpty()) {
                        beginLineTravel(node, process.work_id, *target_route, LineTravelLeg::Outbound);
                    }
                }
            }
            if (!QApplication::isEffectEnabled(Qt::UI_AnimateCombo)) {
                snapToFinalGeometry(process.key, node);
            }
        }

        nodes[QString::fromLatin1(kInputProcessKey)].moving_item->hide();
        nodes[QString::fromLatin1(kVisionProcessKey)].moving_item->hide();
        nodes[QString::fromLatin1(kSortingProcessKey)].moving_item->hide();
        gripper_product->hide();
        QHash<QString, QPair<int, QGraphicsItem*>> product_owners;
        const auto claim_product = [&product_owners](const QString& work_id, int priority, QGraphicsItem* product) {
            if (work_id.isEmpty()) {
                return;
            }
            const auto owner = product_owners.constFind(work_id);
            if (owner == product_owners.cend() || owner->first < priority) {
                product_owners.insert(work_id, { priority, product });
            }
        };
        for (const auto& process : processes) {
            const auto node = nodes.constFind(process.key);
            if (node == nodes.cend() || !HasLiveTelemetry(node->visual) || process.work_id.isEmpty()) {
                continue;
            }
            if (process.key == QString::fromLatin1(kInputProcessKey) &&
                (node->visual.state == FactoryNodeVisualState::Running || DetectedSensor(node->visual).has_value())) {
                claim_product(process.work_id, 0, node->moving_item);
            } else if (process.key == QString::fromLatin1(kVisionProcessKey) &&
                       node->visual.motion_phase == FactoryMotionPhase::VisionProcessing) {
                claim_product(process.work_id, 10, node->moving_item);
            } else if (process.key == QString::fromLatin1(kGripperProcessKey)) {
                const bool prior_stage_matches = process.work_id == input_work_id || process.work_id == vision_work_id;
                const bool sorting_matches = process.work_id == sorting_work_id;
                if (node->visual.motion_phase == FactoryMotionPhase::GripperPick && prior_stage_matches) {
                    claim_product(process.work_id, 20, gripper_product);
                } else if (node->visual.motion_phase == FactoryMotionPhase::GripperTransfer && prior_stage_matches) {
                    claim_product(process.work_id, 30, gripper_product);
                } else if (node->visual.motion_phase == FactoryMotionPhase::GripperPlaced &&
                           (sorting_work_id.isEmpty() ? prior_stage_matches : sorting_matches)) {
                    claim_product(process.work_id, 40, gripper_product);
                }
            } else if (process.key == QString::fromLatin1(kSortingProcessKey) &&
                       (node->visual.state == FactoryNodeVisualState::Working ||
                        node->visual.state == FactoryNodeVisualState::Running)) {
                claim_product(process.work_id, 50, node->moving_item);
            }
        }
        for (const auto& owner : product_owners) {
            owner.second->show();
        }
        updateSelection();
    }

    static qreal pulseOpacity(int phase, qreal minimum) {
        const int step = phase % 12;
        const qreal amount = step <= 6 ? static_cast<qreal>(step) / 6.0 : static_cast<qreal>(12 - step) / 6.0;
        return minimum + ((1.0 - minimum) * amount);
    }

    void tick() {
        const bool animation_enabled = QApplication::isEffectEnabled(Qt::UI_AnimateCombo);
        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) {
            auto& node = iterator.value();
            ++node.pulse_phase;
            if (animation_enabled && node.visual.state == FactoryNodeVisualState::Working) {
                node.group->setOpacity(pulseOpacity(node.pulse_phase, 0.65));
            } else if (animation_enabled && node.visual.state == FactoryNodeVisualState::EmergencyStop) {
                node.group->setOpacity(pulseOpacity(node.pulse_phase, 0.55));
            } else {
                node.group->setOpacity(node.visual.opacity);
            }

            if (!node.motion_enabled) {
                continue;
            }
            if (!animation_enabled) {
                snapToFinalGeometry(iterator.key(), node);
                continue;
            }
            if (iterator.key() == QString::fromLatin1(kInputProcessKey)) {
                if (DetectedSensor(node.visual).has_value()) {
                    node.moving_item->setPos(kInputPositions[3]);
                } else {
                    ++node.animation_phase;
                    const int position = node.animation_phase % 3;
                    node.moving_item->setPos(kInputPositions[position]);
                }
            } else if (iterator.key() == QString::fromLatin1(kSortingProcessKey)) {
                if (const auto sensor = DetectedSensor(node.visual);
                    sensor.has_value() && *sensor >= 1 && *sensor <= 3) {
                    node.moving_item->setPos(kSortingPositions[*sensor - 1]);
                } else {
                    ++node.animation_phase;
                    const int position = node.animation_phase % 3;
                    node.moving_item->setPos(kSortingPositions[position]);
                }
            } else if (iterator.key() == QString::fromLatin1(kLineTracerProcessKey) && !line_path.isEmpty()) {
                if (line_path_index + 1 < line_path.size()) {
                    ++line_path_index;
                    node.moving_item->setPos(line_path[line_path_index]);
                }
            }
        }
    }

    void updateSelection() {
        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) {
            iterator->selection_outline->setVisible(!selected_device_id.isEmpty() &&
                                                    iterator->visual.device_id == selected_device_id);
        }
    }

    FactoryTopViewWidget* owner;
    QGraphicsScene* scene;
    QTimer timer;
    QHash<QString, NodeItems> nodes;
    QString selected_device_id;
    QGraphicsLineItem* gripper_arm{ nullptr };
    QGraphicsLineItem* gripper_jaw_left{ nullptr };
    QGraphicsLineItem* gripper_jaw_right{ nullptr };
    QGraphicsRectItem* gripper_product{ nullptr };
    QGraphicsLineItem* sorting_servo{ nullptr };
    qreal gripper_angle{ 0.0 };
    qreal sorting_servo_angle{ 0.0 };
    std::optional<int> sorting_target_route;
    std::optional<int> line_origin_route;
    std::optional<int> line_target_route;
    LineTravelLeg line_travel_leg{ LineTravelLeg::Idle };
    QList<QPointF> line_path;
    qsizetype line_path_index{ 0 };
    QString line_work_id;
};

FactoryTopViewWidget::FactoryTopViewWidget(QWidget* parent)
    : QGraphicsView(parent), impl_(std::make_unique<Impl>(this)) {}

FactoryTopViewWidget::~FactoryTopViewWidget() = default;

void FactoryTopViewWidget::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    fitInView(sceneRect(), Qt::KeepAspectRatio);
}

void FactoryTopViewWidget::setProcesses(const QList<ProcessUnitStatus>& processes, OverallProcessState overall_state) {
    impl_->setProcesses(processes, overall_state);
}

void FactoryTopViewWidget::setSelectedDeviceId(const QString& device_id) {
    impl_->selected_device_id = device_id;
    impl_->updateSelection();
}

QString FactoryTopViewWidget::selectedDeviceId() const {
    return impl_->selected_device_id;
}

qreal FactoryTopViewWidget::nodeOpacity(const QString& process_key) const {
    const auto iterator = impl_->nodes.constFind(process_key);
    return iterator == impl_->nodes.cend() ? 0.0 : iterator->group->opacity();
}

QColor FactoryTopViewWidget::nodeColor(const QString& process_key) const {
    const auto iterator = impl_->nodes.constFind(process_key);
    return iterator == impl_->nodes.cend() ? QColor{} : iterator->color;
}

QString FactoryTopViewWidget::sensorText(const QString& process_key, int sensor_id) const {
    const auto iterator = impl_->nodes.constFind(process_key);
    return iterator == impl_->nodes.cend() ? QString{} : iterator->sensor_text.value(sensor_id);
}

QPointF FactoryTopViewWidget::boxPosition(const QString& process_key) const {
    const auto iterator = impl_->nodes.constFind(process_key);
    return iterator == impl_->nodes.cend() || iterator->moving_item == nullptr ? QPointF{}
                                                                               : iterator->moving_item->pos();
}

QPointF FactoryTopViewWidget::lineTracerPickupPosition(int route) const {
    return route >= 1 && route <= 3 ? kSortingPositions[route - 1] : QPointF{};
}

QPointF FactoryTopViewWidget::lineTracerJunctionPosition(int route) const {
    return route >= 1 && route <= 3 ? kLineIntersections[route - 1] : QPointF{};
}

QPointF FactoryTopViewWidget::lineTracerDestinationPosition(int route) const {
    return route >= 1 && route <= 3 ? kLineDestinations[route - 1] : QPointF{};
}

qreal FactoryTopViewWidget::gripperAngle() const {
    return impl_->gripper_angle;
}

bool FactoryTopViewWidget::gripperProductVisible() const {
    return impl_->gripper_product->isVisible();
}

QPointF FactoryTopViewWidget::gripperProductPosition() const {
    return impl_->gripper_product->pos();
}

qreal FactoryTopViewWidget::sortingServoAngle() const {
    return impl_->sorting_servo_angle;
}

void FactoryTopViewWidget::advanceAnimationsForTest() {
    impl_->tick();
}

void FactoryTopViewWidget::selectProcessForTest(const QString& process_key) {
    selectProcess(process_key);
}

void FactoryTopViewWidget::selectProcess(const QString& process_key) {
    const auto iterator = impl_->nodes.constFind(process_key);
    if (iterator == impl_->nodes.cend() || iterator->visual.device_id.isEmpty()) {
        return;
    }
    setSelectedDeviceId(iterator->visual.device_id);
    emit controlTargetSelected(iterator->visual.device_id, iterator->display_name);
}

}  // namespace logistics::control_center
