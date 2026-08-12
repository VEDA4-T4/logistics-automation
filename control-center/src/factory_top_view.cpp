#include "logistics/control_center/factory_top_view.hpp"

#include <QApplication>
#include <QBrush>
#include <QFocusEvent>
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsObject>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QHash>
#include <QKeyEvent>
#include <QLineF>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QTimer>
#include <functional>

#include "logistics/contracts/device.hpp"

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

contracts::DeviceStateMeaning StateMeaning(const ProcessUnitStatus& process, const QString& state) {
    const auto role = contracts::DeviceRoleFromString(process.key.toStdString());
    return role.has_value() ? contracts::DeviceStateMeaningFor(*role, state.toStdString())
                            : contracts::DeviceStateMeaning::kUnknown;
}

bool UsesRunningVisual(const ProcessUnitStatus& process, const QString& state) {
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
    visual.opacity = 0.4;
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

FactoryNodeVisual RecoveryVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::Recovery;
    visual.opacity = 1.0;
    return visual;
}

FactoryNodeVisual StoppedVisual(const ProcessUnitStatus& process) {
    auto visual = BaseVisual(process);
    visual.state = FactoryNodeVisualState::Stopped;
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
    const auto meaning = StateMeaning(process, state);
    if (process.connection_state != logistics::contracts::mqtt::ConnectionState::kOnline ||
        state == QStringLiteral("DISCONNECTED")) {
        return DisconnectedVisual(process);
    }
    if (meaning == contracts::DeviceStateMeaning::kEmergencyStop) {
        return EmergencyVisual(process);
    }
    if (process.has_error || meaning == contracts::DeviceStateMeaning::kError) {
        return ErrorVisual(process);
    }
    if (meaning == contracts::DeviceStateMeaning::kRecovery) {
        return RecoveryVisual(process);
    }
    if (meaning == contracts::DeviceStateMeaning::kStopped) {
        return StoppedVisual(process);
    }
    if (UsesRunningVisual(process, state)) {
        return RunningVisual(process);
    }
    if ((process.work_completed && process.key == QString::fromLatin1(kLineTracerProcessKey)) ||
        meaning == contracts::DeviceStateMeaning::kWorking ||
        (meaning == contracts::DeviceStateMeaning::kCompleted &&
         process.key == QString::fromLatin1(kGripperProcessKey))) {
        return WorkingVisual(process, state);
    }
    return WaitingVisual(process);
}

QColor FactoryNodeColor(FactoryNodeVisualState state) {
    switch (state) {
        case FactoryNodeVisualState::Disconnected:
            return QColor(QStringLiteral("#777777"));
        case FactoryNodeVisualState::EmergencyStop:
            return QColor(QStringLiteral("#ff3b30"));
        case FactoryNodeVisualState::Error:
            return QColor(QStringLiteral("#f14c4c"));
        case FactoryNodeVisualState::Recovery:
            return QColor(QStringLiteral("#75beff"));
        case FactoryNodeVisualState::Stopped:
            return QColor(QStringLiteral("#cca700"));
        case FactoryNodeVisualState::Working:
            return QColor(QStringLiteral("#75beff"));
        case FactoryNodeVisualState::Running:
            return QColor(QStringLiteral("#89d185"));
        case FactoryNodeVisualState::Waiting:
            return QColor(QStringLiteral("#ffffff"));
    }
    return QColor(QStringLiteral("#9d9d9d"));
}

namespace {

constexpr QRectF kFactoryScene{ 0, 0, 700, 500 };
constexpr QPointF kInputPositions[]{ { 80, 81 }, { 195, 81 }, { 310, 81 }, { 425, 81 } };
constexpr QPointF kVisionPosition{ 440, 81 };
constexpr QRectF kVisionSelectionRect{ 335, 30, 119, 81 };
constexpr QPointF kGripperPivot{ 504, 81 };
constexpr qreal kGripperReach = 42.0;
constexpr QRectF kGripperSelectionRect{ 464, 44, 49, 79 };
constexpr QPointF kSortingFeed{ 504, 137 };
constexpr QPointF kSortingPositions[]{ { 504, 250 }, { 504, 345 }, { 504, 442 } };
constexpr QPointF kLineIntersections[]{ { 292, 250 }, { 292, 345 }, { 292, 442 } };
constexpr QPointF kLineDestinations[]{ { 80, 250 }, { 80, 345 }, { 80, 442 } };
constexpr QPointF kLineTracerSensorPositions[]{ { 80, 165 }, { 195, 165 }, { 80, 183 }, { 195, 183 } };
constexpr QRectF kLineTracerSelectionRect{ 70, 235, 445, 222 };

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
        return kSortingPositions[*route];
    }
    if (area == QStringLiteral("DESTINATION")) {
        return kLineDestinations[*route];
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
        setFlag(QGraphicsItem::ItemIsFocusable);
        setObjectName(QStringLiteral("factoryProcessNode"));
        setProperty("processKey", process_key_);
    }

    void setFocusOutlineRect(const QRectF& rect) {
        focus_outline_rect_ = rect;
        update();
    }

    [[nodiscard]] QRectF boundingRect() const override {
        return childrenBoundingRect().adjusted(-4, -4, 4, 4);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        if (!hasFocus() || !focus_outline_visible_) {
            return;
        }
        QPen focus_pen(QColor(QStringLiteral("#d7ba7d")), 2, Qt::DashLine);
        focus_pen.setCosmetic(true);
        painter->setPen(focus_pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(focus_outline_rect_.value_or(boundingRect()), 4, 4);
    }

protected:
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            selected_(process_key_);
            event->accept();
            return;
        }
        QGraphicsObject::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
            selected_(process_key_);
            event->accept();
            return;
        }
        QGraphicsObject::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent* event) override {
        focus_outline_visible_ = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason ||
                                 event->reason() == Qt::ShortcutFocusReason;
        QGraphicsObject::focusInEvent(event);
        update();
    }

    void focusOutEvent(QFocusEvent* event) override {
        focus_outline_visible_ = false;
        QGraphicsObject::focusOutEvent(event);
        update();
    }

private:
    QString process_key_;
    std::function<void(const QString&)> selected_;
    std::optional<QRectF> focus_outline_rect_;
    bool focus_outline_visible_{ false };
};

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
    struct NodeItems {
        ProcessGraphicsGroup* group{ nullptr };
        QGraphicsEllipseItem* state_marker{ nullptr };
        QGraphicsSimpleTextItem* display_label{ nullptr };
        QGraphicsRectItem* selection_outline{ nullptr };
        QGraphicsItem* moving_item{ nullptr };
        QList<QGraphicsLineItem*> state_lines;
        QList<QAbstractGraphicsShapeItem*> state_shapes;
        QHash<int, QGraphicsSimpleTextItem*> sensor_labels;
        QHash<int, QString> sensor_label_prefixes;
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
        owner->setFocusPolicy(Qt::StrongFocus);
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
        node.display_label = new QGraphicsSimpleTextItem(display_name, node.group);
        node.display_label->setBrush(QColor(QStringLiteral("#cccccc")));
        node.display_label->setPos(label_position + QPointF(15, -4));
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

    static void finalizeNode(NodeItems& node, std::optional<QRectF> selection_rect = std::nullopt) {
        for (auto* child : node.group->childItems()) {
            child->setAcceptedMouseButtons(Qt::NoButton);
        }
        const auto outline_rect = selection_rect.value_or(node.group->childrenBoundingRect()).adjusted(-5, -5, 5, 5);
        node.selection_outline = new QGraphicsRectItem(outline_rect, node.group);
        if (selection_rect.has_value()) {
            node.group->setFocusOutlineRect(outline_rect);
        }
        QPen selection_pen(QColor(QStringLiteral("#4daafc")), 2);
        selection_pen.setCosmetic(true);
        node.selection_outline->setPen(selection_pen);
        node.selection_outline->setBrush(Qt::NoBrush);
        node.selection_outline->setZValue(100);
        node.selection_outline->setAcceptedMouseButtons(Qt::NoButton);
        node.selection_outline->hide();
        node.group->scene()->update(node.group->boundingRect());
    }

    void alignSortingLabels(NodeItems& node) {
        if (sorting_label_right <= 0.0) {
            return;
        }
        auto heading_position = node.display_label->pos();
        heading_position.setX(sorting_label_right - node.display_label->boundingRect().right());
        node.display_label->setPos(heading_position);
        auto marker_rect = node.state_marker->rect();
        marker_rect.moveLeft(heading_position.x() - 15.0);
        node.state_marker->setRect(marker_rect);
        for (auto* sensor_label : node.sensor_labels) {
            auto sensor_position = sensor_label->pos();
            sensor_position.setX(sorting_label_right - sensor_label->boundingRect().right());
            sensor_label->setPos(sensor_position);
        }
    }

    void buildScene() {
        auto& input = addNode(QString::fromLatin1(kInputProcessKey), QStringLiteral("Input"), QPointF(80, 35));
        addStateLine(input, QLineF(kInputPositions[0], kInputPositions[3]), 6);
        addBox(input, kInputPositions[0]);
        auto* input_sensor = new QGraphicsSimpleTextItem(QStringLiteral("US1 -- cm"), input.group);
        input_sensor->setBrush(QColor(QStringLiteral("#cccccc")));
        input_sensor->setPos(356, 92);
        input.sensor_labels.insert(1, input_sensor);
        scene->addItem(input.group);
        finalizeNode(input);

        auto& vision = addNode(QString::fromLatin1(kVisionProcessKey), QStringLiteral("Vision"), QPointF(335, 35));
        auto* camera = new QGraphicsRectItem(QRectF(kVisionPosition - QPointF(15, 15), QSizeF(30, 30)), vision.group);
        camera->setBrush(QColor(QStringLiteral("#2d2d30")));
        camera->setPen(QPen(vision.color, 3));
        vision.state_shapes.append(camera);
        addStateLine(vision, QLineF(kVisionPosition - QPointF(9, 7), kVisionPosition + QPointF(9, 7)), 3);
        addBox(vision, kVisionPosition);
        scene->addItem(vision.group);
        finalizeNode(vision, kVisionSelectionRect);

        auto& gripper = addNode(QString::fromLatin1(kGripperProcessKey), QStringLiteral("Gripper"), QPointF(462, 48));
        gripper.state_marker->setZValue(10);
        auto* pivot =
            new QGraphicsEllipseItem(QRectF(kGripperPivot.x() - 8, kGripperPivot.y() - 8, 16, 16), gripper.group);
        pivot->setBrush(QColor(QStringLiteral("#2d2d30")));
        pivot->setPen(QPen(gripper.color, 3));
        gripper.state_shapes.append(pivot);
        const auto gripper_pickup_position = kGripperPivot - QPointF(kGripperReach, 0);
        gripper_arm = addStateLine(gripper, QLineF(kGripperPivot, gripper_pickup_position), 7);
        gripper_jaw_left =
            addStateLine(gripper, QLineF(gripper_pickup_position - QPointF(0, 8), gripper_pickup_position), 3);
        gripper_jaw_right =
            addStateLine(gripper, QLineF(gripper_pickup_position, gripper_pickup_position + QPointF(0, 8)), 3);
        gripper_product = new QGraphicsRectItem(QRectF(-8, -8, 16, 16), gripper.group);
        gripper_product->setBrush(QColor(QStringLiteral("#ce9178")));
        gripper_product->setPen(QPen(QColor(QStringLiteral("#f0f0f0")), 1));
        gripper_product->setPos(gripper_pickup_position);
        gripper_product->hide();
        scene->addItem(gripper.group);
        finalizeNode(gripper, kGripperSelectionRect);

        auto& sorting = addNode(QString::fromLatin1(kSortingProcessKey), QStringLiteral("Sorting"), QPointF(535, 185));
        addStateLine(sorting, QLineF(kSortingFeed, kSortingPositions[2]), 6);
        addBox(sorting, kSortingPositions[0]);
        for (int sensor_id = 1; sensor_id <= 3; ++sensor_id) {
            auto* sensor = new QGraphicsSimpleTextItem(QStringLiteral("US%1 -- cm").arg(sensor_id + 1), sorting.group);
            sensor->setBrush(QColor(QStringLiteral("#cccccc")));
            sensor->setPos(kSortingPositions[sensor_id - 1] + QPointF(18, -10));
            if (sensor_id == 1) {
                sorting_label_right = sensor->pos().x() + sensor->boundingRect().right();
            }
            sorting.sensor_labels.insert(sensor_id, sensor);
        }
        alignSortingLabels(sorting);
        sorting_servo = addStateLine(sorting, QLineF(485, 297, 505, 297), 5);
        scene->addItem(sorting.group);
        finalizeNode(sorting);

        auto& line_tracer =
            addNode(QString::fromLatin1(kLineTracerProcessKey), QStringLiteral("Line tracer"), QPointF(80, 205));
        const QString sensor_directions[]{ QStringLiteral("전"), QStringLiteral("후"), QStringLiteral("좌"),
                                           QStringLiteral("우") };
        for (int sensor_index = 0; sensor_index < 4; ++sensor_index) {
            const auto sensor_id = sensor_index + 1;
            const auto& direction = sensor_directions[sensor_index];
            auto* sensor = new QGraphicsSimpleTextItem(QStringLiteral("%1 -- cm").arg(direction), line_tracer.group);
            sensor->setBrush(QColor(QStringLiteral("#cccccc")));
            sensor->setPos(kLineTracerSensorPositions[sensor_index]);
            line_tracer.sensor_labels.insert(sensor_id, sensor);
            line_tracer.sensor_label_prefixes.insert(sensor_id, direction);
        }
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
        for (int index = 0; index < 8; ++index) {
            auto* arrow = new QGraphicsSimpleTextItem(QStringLiteral(">"), line_tracer.group);
            QFont font = arrow->font();
            font.setWeight(QFont::Black);
            font.setPointSize(16);
            arrow->setFont(font);
            arrow->setBrush(QColor(QStringLiteral("#ffffff")));
            arrow->setTransformOriginPoint(arrow->boundingRect().center());
            arrow->hide();
            line_arrows.append(arrow);
        }
        scene->addItem(line_tracer.group);
        finalizeNode(line_tracer, kLineTracerSelectionRect);

        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) {
            applyVisual(iterator.value());
        }
    }

    void applyVisual(NodeItems& node) {
        node.color = FactoryNodeColor(node.visual.state);
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
            const auto fallback_number = iterator.key() + (node.sensor_labels.size() == 1 ? 0 : 1);
            const auto prefix =
                node.sensor_label_prefixes.value(iterator.key(), QStringLiteral("US%1").arg(fallback_number));
            node.sensor_text.insert(iterator.key(), QStringLiteral("-- cm"));
            iterator.value()->setText(QStringLiteral("%1 -- cm").arg(prefix));
        }
        if (node.sensor_labels.size() == 3) {
            alignSortingLabels(node);
        }
        if (node.visual.state == FactoryNodeVisualState::Disconnected) {
            return;
        }
        for (const auto& sensor : node.visual.sensors) {
            node.sensor_text.insert(sensor.sensor_id, sensor.distance_text);
            if (auto* label = node.sensor_labels.value(sensor.sensor_id, nullptr); label != nullptr) {
                const auto fallback_number = sensor.sensor_id + (node.sensor_labels.size() == 1 ? 0 : 1);
                const auto prefix =
                    node.sensor_label_prefixes.value(sensor.sensor_id, QStringLiteral("US%1").arg(fallback_number));
                label->setText(QStringLiteral("%1 %2").arg(prefix, sensor.distance_text));
            }
        }
        if (node.sensor_labels.size() == 3) {
            alignSortingLabels(node);
        }
    }

    void applyGripperPhase(FactoryMotionPhase phase) {
        if (phase != FactoryMotionPhase::GripperPick && phase != FactoryMotionPhase::GripperTransfer &&
            phase != FactoryMotionPhase::GripperPlaced) {
            return;
        }
        gripper_angle = phase == FactoryMotionPhase::GripperPick ? 0.0 : 90.0;
        const bool open = phase == FactoryMotionPhase::GripperPlaced;
        const QPointF end = gripper_angle == 0.0 ? kGripperPivot - QPointF(kGripperReach, 0)
                                                 : kGripperPivot + QPointF(0, kGripperReach);
        gripper_arm->setLine(QLineF(kGripperPivot, end));
        if (gripper_angle == 0.0) {
            const qreal spread = open ? 8.0 : 3.0;
            gripper_jaw_left->setLine(QLineF(end.x(), end.y() - spread, end.x(), end.y()));
            gripper_jaw_right->setLine(QLineF(end.x(), end.y(), end.x(), end.y() + spread));
        } else {
            const qreal spread = open ? 8.0 : 3.0;
            gripper_jaw_left->setLine(QLineF(end.x() - spread, end.y(), end.x(), end.y()));
            gripper_jaw_right->setLine(QLineF(end.x(), end.y(), end.x() + spread, end.y()));
        }
    }

    void applyGripperProduct(FactoryMotionPhase phase, bool visible) {
        gripper_product->setVisible(visible);
        if (!visible) {
            return;
        }
        if (phase == FactoryMotionPhase::GripperPick) {
            gripper_product->setPos(kGripperPivot - QPointF(kGripperReach, 0));
        } else if (phase == FactoryMotionPhase::GripperTransfer) {
            gripper_product->setPos(kGripperPivot + QPointF(0, kGripperReach));
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
        } else if (process_key == QString::fromLatin1(kLineTracerProcessKey)) {
            updateLineArrows();
        }
    }

    void beginLineTravel(NodeItems& node, const QString& work_id, int target_route) {
        const int origin_route = line_origin_route.value_or(target_route);
        line_work_id = work_id;
        line_target_route = target_route;
        line_path.clear();

        line_path.append(kSortingPositions[origin_route - 1]);
        const int step = origin_route <= target_route ? 1 : -1;
        for (int route = origin_route;; route += step) {
            line_path.append(kLineIntersections[route - 1]);
            if (route == target_route) {
                break;
            }
        }
        line_path.append(kLineDestinations[target_route - 1]);
        node.moving_item->setPos(line_path.front());
        line_arrow_phase = 0;
        updateLineArrows();
    }

    void hideLineArrows() {
        for (auto* arrow : line_arrows) {
            arrow->hide();
        }
    }

    void updateLineArrows() {
        if (line_path.size() < 2) {
            hideLineArrows();
            return;
        }
        qreal total_length = 0.0;
        for (qsizetype index = 1; index < line_path.size(); ++index) {
            total_length += QLineF(line_path[index - 1], line_path[index]).length();
        }
        for (int arrow_index = 0; arrow_index < line_arrows.size(); ++arrow_index) {
            qreal remaining = total_length * static_cast<qreal>((line_arrow_phase + arrow_index * 12) % 100) / 100.0;
            for (qsizetype path_index = 1; path_index < line_path.size(); ++path_index) {
                const QLineF segment(line_path[path_index - 1], line_path[path_index]);
                if (remaining <= segment.length() || path_index + 1 == line_path.size()) {
                    auto* arrow = line_arrows[arrow_index];
                    arrow->setPos(segment.pointAt(segment.length() > 0.0 ? remaining / segment.length() : 0.0) -
                                  arrow->boundingRect().center());
                    arrow->setRotation(-segment.angle());
                    arrow->show();
                    break;
                }
                remaining -= segment.length();
            }
        }
    }

    void setProcesses(const QList<ProcessUnitStatus>& processes, OverallProcessState) {
        const auto visual_for = [](const ProcessUnitStatus& process) { return BuildFactoryNodeVisual(process); };
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
                    line_path.clear();
                    hideLineArrows();
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
                            line_work_id = process.work_id;
                            line_arrow_phase = 0;
                        }
                        node.motion_enabled =
                            NormalizedState(process.current_state) == QStringLiteral("FOLLOWING_LINE") &&
                            line_path.size() > 1;
                        node.moving_item->setPos(*confirmed_point);
                        updateLineArrows();
                    } else if (confirmed_point.has_value()) {
                        line_path.clear();
                        line_work_id = process.work_id;
                        node.motion_enabled = false;
                        node.moving_item->setPos(*confirmed_point);
                        hideLineArrows();
                    }
                    continue;
                }
                if (NormalizedState(process.current_state) == QStringLiteral("POSITION_UNKNOWN")) {
                    node.motion_enabled = false;
                    node.moving_item->hide();
                    line_path.clear();
                    hideLineArrows();
                    continue;
                }
                node.moving_item->show();

                const auto state = NormalizedState(process.current_state);
                const auto arrived_route = RouteSuffix(state, u"ARRIVED_");
                const auto unloading_route = RouteSuffix(state, u"UNLOADING_");
                const auto pickup_route = RouteSuffix(state, u"PICKUP_READY_");
                if (const auto route = arrived_route.has_value() ? arrived_route : unloading_route; route.has_value()) {
                    line_origin_route.reset();
                    line_target_route = route;
                    line_path.clear();
                    line_work_id = process.work_id;
                    node.motion_enabled = false;
                    node.moving_item->setPos(kLineDestinations[*route - 1]);
                    hideLineArrows();
                } else if (pickup_route.has_value()) {
                    line_origin_route = pickup_route;
                    line_target_route.reset();
                    line_work_id = process.work_id;
                    line_path.clear();
                    node.motion_enabled = false;
                    node.moving_item->setPos(kSortingPositions[*pickup_route - 1]);
                    hideLineArrows();
                } else if (node.visual.motion_phase == FactoryMotionPhase::LineCompleted) {
                    if (const auto route = FactoryRouteIndex(process.destination); route.has_value()) {
                        line_origin_route.reset();
                        line_target_route = route;
                        line_path.clear();
                        line_work_id = process.work_id;
                        node.moving_item->setPos(kLineDestinations[*route - 1]);
                        hideLineArrows();
                    }
                    node.motion_enabled = false;
                } else if (state == QStringLiteral("FOLLOWING_LINE") || state == QStringLiteral("STOPPED") ||
                           state == QStringLiteral("DELIVERING")) {
                    const auto target_route = FactoryRouteIndex(process.destination);
                    if (!target_route.has_value()) {
                        node.motion_enabled = false;
                        line_target_route.reset();
                        line_path.clear();
                        hideLineArrows();
                    } else if (line_work_id != process.work_id || line_target_route != target_route ||
                               line_path.isEmpty()) {
                        beginLineTravel(node, process.work_id, *target_route);
                    }
                    node.motion_enabled = state == QStringLiteral("FOLLOWING_LINE") && line_path.size() > 1;
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
                ++node.animation_phase;
                if (DetectedSensor(node.visual).has_value()) {
                    node.moving_item->setPos(kInputPositions[3]);
                } else {
                    const int position = node.animation_phase % 3;
                    node.moving_item->setPos(kInputPositions[position]);
                }
            } else if (iterator.key() == QString::fromLatin1(kSortingProcessKey)) {
                ++node.animation_phase;
                if (const auto sensor = DetectedSensor(node.visual);
                    sensor.has_value() && *sensor >= 1 && *sensor <= 3) {
                    node.moving_item->setPos(kSortingPositions[*sensor - 1]);
                } else {
                    const int position = node.animation_phase % 3;
                    node.moving_item->setPos(kSortingPositions[position]);
                }
            } else if (iterator.key() == QString::fromLatin1(kLineTracerProcessKey) && !line_path.isEmpty()) {
                line_arrow_phase = (line_arrow_phase + 4) % 100;
                updateLineArrows();
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
    qreal sorting_label_right{ 0.0 };
    QList<QGraphicsSimpleTextItem*> line_arrows;
    qreal gripper_angle{ 0.0 };
    qreal sorting_servo_angle{ 0.0 };
    std::optional<int> sorting_target_route;
    std::optional<int> line_origin_route;
    std::optional<int> line_target_route;
    QList<QPointF> line_path;
    int line_arrow_phase{ 0 };
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

QList<QPointF> FactoryTopViewWidget::lineArrowPositions() const {
    QList<QPointF> positions;
    for (auto* arrow : impl_->line_arrows) {
        if (arrow->isVisible()) {
            positions.append(arrow->pos());
        }
    }
    return positions;
}

QRectF FactoryTopViewWidget::nodeSelectionRect(const QString& process_key) const {
    const auto iterator = impl_->nodes.constFind(process_key);
    return iterator == impl_->nodes.cend() ? QRectF{} : iterator->selection_outline->rect();
}

qreal FactoryTopViewWidget::gripperAngle() const {
    return impl_->gripper_angle;
}

qreal FactoryTopViewWidget::gripperArmLength() const {
    return impl_->gripper_arm->line().length();
}

QPointF FactoryTopViewWidget::gripperEndPosition() const {
    return impl_->gripper_arm->line().p2();
}

QPointF FactoryTopViewWidget::gripperPivotPosition() const {
    return kGripperPivot;
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
