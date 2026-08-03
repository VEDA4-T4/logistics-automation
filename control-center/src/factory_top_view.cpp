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
#include <QTimer>
#include <functional>

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

namespace {

constexpr QRectF kFactoryScene{ 0, 0, 700, 500 };
constexpr QPointF kInputPositions[]{ { 78, 69 }, { 174, 69 }, { 270, 69 }, { 375, 81 } };
constexpr QPointF kSortingPositions[]{ { 525, 126 }, { 525, 178 }, { 525, 230 } };
constexpr QPointF kGripperPivot{ 440, 145 };
constexpr QPointF kLineStarts[]{ { 500, 250 }, { 500, 345 }, { 500, 442 } };
constexpr QPointF kLineIntersections[]{ { 292, 250 }, { 292, 345 }, { 292, 442 } };
constexpr QPointF kLineDestinations[]{ { 58, 250 }, { 58, 345 }, { 58, 442 } };

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
            return QColor(QStringLiteral("#9d9d9d"));
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
        QGraphicsRectItem* selection_outline{ nullptr };
        QGraphicsItem* moving_item{ nullptr };
        QList<QGraphicsLineItem*> state_lines;
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
        addStateLine(input, QLineF(kInputPositions[0], kInputPositions[2]), 6);
        addBox(input, kInputPositions[0]);
        auto* input_sensor = new QGraphicsSimpleTextItem(QStringLiteral("US1 -- cm"), input.group);
        input_sensor->setBrush(QColor(QStringLiteral("#cccccc")));
        input_sensor->setPos(356, 92);
        input.sensor_labels.insert(1, input_sensor);
        scene->addItem(input.group);
        finalizeNode(input);

        auto& vision = addNode(QString::fromLatin1(kVisionProcessKey), QStringLiteral("Vision"), QPointF(335, 35));
        auto* camera = new QGraphicsRectItem(QRectF(360, 66, 30, 30), vision.group);
        camera->setBrush(QColor(QStringLiteral("#2d2d30")));
        camera->setPen(QPen(vision.color, 3));
        addStateLine(vision, QLineF(366, 74, 384, 88), 3);
        scene->addItem(vision.group);
        finalizeNode(vision);

        auto& gripper = addNode(QString::fromLatin1(kGripperProcessKey), QStringLiteral("Gripper"), QPointF(405, 105));
        auto* pivot =
            new QGraphicsEllipseItem(QRectF(kGripperPivot.x() - 8, kGripperPivot.y() - 8, 16, 16), gripper.group);
        pivot->setBrush(QColor(QStringLiteral("#2d2d30")));
        pivot->setPen(QPen(gripper.color, 3));
        gripper_arm = addStateLine(gripper, QLineF(kGripperPivot, kGripperPivot - QPointF(0, 42)), 7);
        gripper_jaw_left = addStateLine(gripper, QLineF(432, 103, 440, 103), 3);
        gripper_jaw_right = addStateLine(gripper, QLineF(440, 103, 448, 103), 3);
        scene->addItem(gripper.group);
        finalizeNode(gripper);

        auto& sorting = addNode(QString::fromLatin1(kSortingProcessKey), QStringLiteral("Sorting"), QPointF(530, 85));
        addStateLine(sorting, QLineF(kSortingPositions[0], kSortingPositions[2]), 6);
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
            addStateLine(line_tracer, QLineF(kLineStarts[route], kLineIntersections[route]), 3);
            addStateLine(line_tracer, QLineF(kLineIntersections[route], kLineDestinations[route]), 3);
        }
        auto* tracer = new QGraphicsEllipseItem(QRectF(-7, -7, 14, 14), line_tracer.group);
        tracer->setBrush(QColor(QStringLiteral("#dcdcaa")));
        tracer->setPen(QPen(QColor(QStringLiteral("#f0f0f0")), 1));
        tracer->setPos(kLineStarts[0]);
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
        const QPointF end = gripper_angle == 0.0 ? kGripperPivot - QPointF(0, 42) : kGripperPivot + QPointF(42, 0);
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

    void applySortingRoute(const ProcessUnitStatus& process) {
        auto route = FactoryRouteIndex(process.current_state);
        if (!route.has_value()) {
            route = FactoryRouteIndex(process.work_id);
        }
        if (!route.has_value()) {
            return;
        }
        sorting_servo_angle = (*route - 1) * 45.0;
        QLineF servo(QPointF(485, 297), QPointF(505, 297));
        servo.setAngle(-sorting_servo_angle);
        sorting_servo->setLine(servo);
    }

    void setProcesses(const QList<ProcessUnitStatus>& processes) {
        for (const auto& process : processes) {
            auto iterator = nodes.find(process.key);
            if (iterator == nodes.end()) {
                continue;
            }
            auto& node = iterator.value();
            const auto previous_sensor = DetectedSensor(node.visual);
            node.visual = BuildFactoryNodeVisual(process);
            node.motion_enabled = AllowsMotion(process.key, node.visual);
            if (process.key == QString::fromLatin1(kLineTracerProcessKey) &&
                node.visual.motion_phase == FactoryMotionPhase::LineCompleted) {
                node.motion_enabled = false;
            }
            if (!process.display_name.isEmpty()) {
                node.display_name = process.display_name;
            }
            updateSensors(node);
            applyVisual(node);

            if (process.key == QString::fromLatin1(kInputProcessKey)) {
                if (DetectedSensor(node.visual).has_value()) {
                    node.moving_item->setPos(kInputPositions[3]);
                }
            } else if (process.key == QString::fromLatin1(kGripperProcessKey)) {
                applyGripperPhase(node.visual.motion_phase);
            } else if (process.key == QString::fromLatin1(kSortingProcessKey)) {
                if (const auto sensor = DetectedSensor(node.visual);
                    sensor.has_value() && *sensor >= 1 && *sensor <= 3) {
                    node.moving_item->setPos(kSortingPositions[*sensor - 1]);
                } else if (previous_sensor.has_value() && *previous_sensor >= 1 && *previous_sensor <= 3) {
                    node.animation_phase = *previous_sensor - 1;
                }
                applySortingRoute(process);
            } else if (process.key == QString::fromLatin1(kLineTracerProcessKey)) {
                auto route = FactoryRouteIndex(process.current_state);
                if (!route.has_value()) {
                    route = FactoryRouteIndex(process.work_id);
                }
                if (route.has_value()) {
                    if (line_route != *route) {
                        node.animation_phase = 0;
                        node.moving_item->setPos(kLineStarts[*route - 1]);
                    }
                    line_route = *route;
                }
                if (node.visual.motion_phase == FactoryMotionPhase::LineCompleted) {
                    node.moving_item->setPos(kLineDestinations[line_route - 1]);
                }
            }
        }
        updateSelection();
    }

    static qreal pulseOpacity(int phase, qreal minimum) {
        const int step = phase % 12;
        const qreal amount = step <= 6 ? static_cast<qreal>(step) / 6.0 : static_cast<qreal>(12 - step) / 6.0;
        return minimum + ((1.0 - minimum) * amount);
    }

    void tick() {
        const bool pulse_enabled = QApplication::isEffectEnabled(Qt::UI_AnimateCombo);
        for (auto iterator = nodes.begin(); iterator != nodes.end(); ++iterator) {
            auto& node = iterator.value();
            ++node.pulse_phase;
            if (pulse_enabled && node.visual.state == FactoryNodeVisualState::Working) {
                node.group->setOpacity(pulseOpacity(node.pulse_phase, 0.65));
            } else if (pulse_enabled && node.visual.state == FactoryNodeVisualState::EmergencyStop) {
                node.group->setOpacity(pulseOpacity(node.pulse_phase, 0.55));
            } else {
                node.group->setOpacity(node.visual.opacity);
            }

            if (!node.motion_enabled) {
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
            } else if (iterator.key() == QString::fromLatin1(kLineTracerProcessKey)) {
                const int route = line_route - 1;
                ++node.animation_phase;
                const int position = node.animation_phase % 3;
                if (position == 0) {
                    node.moving_item->setPos(kLineStarts[route]);
                } else if (position == 1) {
                    node.moving_item->setPos(kLineIntersections[route]);
                } else {
                    node.moving_item->setPos(kLineDestinations[route]);
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
    QGraphicsLineItem* sorting_servo{ nullptr };
    qreal gripper_angle{ 0.0 };
    qreal sorting_servo_angle{ 0.0 };
    int line_route{ 1 };
};

FactoryTopViewWidget::FactoryTopViewWidget(QWidget* parent)
    : QGraphicsView(parent), impl_(std::make_unique<Impl>(this)) {}

FactoryTopViewWidget::~FactoryTopViewWidget() = default;

void FactoryTopViewWidget::setProcesses(const QList<ProcessUnitStatus>& processes) {
    impl_->setProcesses(processes);
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

qreal FactoryTopViewWidget::gripperAngle() const {
    return impl_->gripper_angle;
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
