#include "logistics/control_center/detection_overlay.hpp"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QVideoSink>
#include <algorithm>
#include <array>
#include <utility>

namespace logistics::control_center {
namespace {

constexpr double kMinimumDangerZoneSize = 0.02;
constexpr double kDangerZoneHandleRadius = 6.0;
constexpr quint64 kArucoFrameInterval = 1;

[[nodiscard]] QRectF ClampedDangerZone(QRectF zone) {
    zone = zone.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    return zone.width() >= kMinimumDangerZoneSize && zone.height() >= kMinimumDangerZoneSize ? zone : QRectF{};
}

}  // namespace

DetectionOverlay::DetectionOverlay(QWidget* parent)
    : QWidget(parent), video_sink_(new QVideoSink(this)), stale_timer_(new QTimer(this)) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    stale_timer_->setSingleShot(true);
    stale_timer_->setInterval(1500);
    connect(stale_timer_, &QTimer::timeout, this, &DetectionOverlay::clearDetections);
    connect(video_sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
        video_frame_ = frame;
        video_size_ = frame.isValid() ? frame.size() : QSize();
        if (frame.isValid() && ++aruco_frame_counter_ % kArucoFrameInterval == 0 && !aruco_detection_in_flight_) {
            queueArucoDetection(frame.toImage());
        }
        update();
    });
    qRegisterMetaType<QList<ArucoMarkerRegion>>();
    aruco_thread_ = new QThread(this);
    aruco_worker_ = new ArucoDetectorWorker(0.05);
    aruco_worker_->moveToThread(aruco_thread_);
    connect(aruco_thread_, &QThread::finished, aruco_worker_, &QObject::deleteLater);
    connect(aruco_worker_, &ArucoDetectorWorker::markersDetected, this,
            [this](const QList<ArucoMarkerRegion>& markers) {
                setArucoMarkers(markers);
                aruco_detection_in_flight_ = false;
            });
    aruco_thread_->start();
}

DetectionOverlay::~DetectionOverlay() {
    aruco_thread_->quit();
    aruco_thread_->wait();
}

void DetectionOverlay::setChannelLabel(const QString& label) {
    channel_label_ = label;
    update();
}

void DetectionOverlay::setDetectionFrame(const OnvifDetectionFrame& frame) {
    frame_ = frame;
    has_frame_ = true;
    stale_timer_->start();
    evaluateDangerZoneOccupancy();
    update();
}

void DetectionOverlay::setMetadataState(bool connected, const QString& detail) {
    metadata_connected_ = connected;
    metadata_detail_ = detail;
    if (!connected) {
        clearDetections();
    } else {
        update();
    }
}

void DetectionOverlay::setStaleTimeout(int timeout_ms) {
    stale_timer_->setInterval(std::max(timeout_ms, 100));
}

void DetectionOverlay::clearDetections() {
    has_frame_ = false;
    frame_.detections.clear();
    occupied_danger_zones_.clear();
    occupied_aruco_markers_.clear();
    emit dangerZoneOccupancyEvaluated(false, -1, {}, 0.0);
    update();
}

QVideoSink* DetectionOverlay::videoSink() const {
    return video_sink_;
}

void DetectionOverlay::setDangerZones(QList<QRectF> zones) {
    danger_zones_.clear();
    for (const auto& zone : zones) {
        const auto clamped = ClampedDangerZone(zone);
        if (!clamped.isEmpty()) {
            danger_zones_.append(clamped);
        }
    }
    if (!danger_zone_editing_) {
        draft_danger_zones_ = danger_zones_;
    }
    evaluateDangerZoneOccupancy();
    update();
}

const QList<QRectF>& DetectionOverlay::dangerZones() const noexcept {
    return danger_zones_;
}

void DetectionOverlay::setDangerZonesVisible(const bool visible) {
    danger_zones_visible_ = visible;
    update();
}

bool DetectionOverlay::dangerZonesVisible() const noexcept {
    return danger_zones_visible_;
}

void DetectionOverlay::setPersonClasses(QStringList classes) {
    QStringList normalized{ QStringLiteral("HEAD"), QStringLiteral("FACE"), QStringLiteral("HUMAN") };
    for (auto& value : classes) {
        value = value.trimmed().toUpper();
        if (!value.isEmpty() && !normalized.contains(value)) {
            normalized.append(value);
        }
    }
    if (!normalized.isEmpty()) {
        person_classes_ = std::move(normalized);
    }
    evaluateDangerZoneOccupancy();
}

void DetectionOverlay::setMinimumPersonConfidence(const double confidence) {
    minimum_person_confidence_ = std::clamp(confidence, 0.0, 1.0);
    evaluateDangerZoneOccupancy();
}

void DetectionOverlay::beginDangerZoneEditing() {
    if (danger_zone_editing_) {
        return;
    }
    draft_danger_zones_ = danger_zones_;
    selected_danger_zone_ = draft_danger_zones_.isEmpty() ? -1 : 0;
    drag_handle_ = DragHandle::None;
    danger_zone_editing_ = true;
    update();
}

void DetectionOverlay::commitDangerZoneEditing() {
    if (!danger_zone_editing_) {
        return;
    }
    danger_zones_ = draft_danger_zones_;
    danger_zone_editing_ = false;
    selected_danger_zone_ = -1;
    drag_handle_ = DragHandle::None;
    evaluateDangerZoneOccupancy();
    update();
}

void DetectionOverlay::cancelDangerZoneEditing() {
    if (!danger_zone_editing_) {
        return;
    }
    draft_danger_zones_ = danger_zones_;
    danger_zone_editing_ = false;
    selected_danger_zone_ = -1;
    drag_handle_ = DragHandle::None;
    update();
}

bool DetectionOverlay::isDangerZoneEditing() const noexcept {
    return danger_zone_editing_;
}

void DetectionOverlay::addDangerZone() {
    if (!danger_zone_editing_) {
        return;
    }
    const double offset = 0.025 * static_cast<double>(draft_danger_zones_.size() % 5);
    draft_danger_zones_.append(QRectF(0.35 + offset, 0.35 + offset, 0.3, 0.3));
    selected_danger_zone_ = draft_danger_zones_.size() - 1;
    update();
}

void DetectionOverlay::deleteSelectedDangerZone() {
    if (!danger_zone_editing_ || selected_danger_zone_ < 0 || selected_danger_zone_ >= draft_danger_zones_.size()) {
        return;
    }
    draft_danger_zones_.removeAt(selected_danger_zone_);
    selected_danger_zone_ = draft_danger_zones_.isEmpty()
                                ? -1
                                : std::min(selected_danger_zone_, static_cast<int>(draft_danger_zones_.size()) - 1);
    drag_handle_ = DragHandle::None;
    update();
}

void DetectionOverlay::setArucoMarkers(const QList<ArucoMarkerRegion>& markers) {
    aruco_markers_.clear();
    occupied_aruco_markers_.clear();
    for (const auto& marker : markers) {
        if (marker.marker_id >= 0 && marker.corners.size() == 4 && !marker.danger_rect.isEmpty()) {
            aruco_markers_.insert(marker.marker_id, marker);
        }
    }
    evaluateDangerZoneOccupancy();
    update();
}

void DetectionOverlay::clearArucoMarkers() {
    aruco_markers_.clear();
    occupied_aruco_markers_.clear();
    evaluateDangerZoneOccupancy();
    update();
}

void DetectionOverlay::queueArucoDetection(QImage image) {
    if (image.isNull()) {
        return;
    }
    if (aruco_detection_in_flight_) {
        return;
    }
    aruco_detection_in_flight_ = true;
    auto* worker = aruco_worker_;
    QMetaObject::invokeMethod(
        worker, [worker, image = std::move(image)]() { worker->detect(image); }, Qt::QueuedConnection);
}

QRectF DetectionOverlay::displayedVideoRect() const {
    const QRectF available(rect());
    if (!video_size_.isValid() || video_size_.isEmpty()) {
        return available;
    }

    const auto scale = std::min(available.width() / video_size_.width(), available.height() / video_size_.height());
    const QSizeF fitted(video_size_.width() * scale, video_size_.height() * scale);
    return QRectF(QPointF((available.width() - fitted.width()) / 2.0, (available.height() - fitted.height()) / 2.0),
                  fitted);
}

QRectF DetectionOverlay::dangerZoneDisplayRect(const QRectF& normalized_zone) const {
    const auto video_rect = displayedVideoRect();
    return {
        video_rect.left() + normalized_zone.left() * video_rect.width(),
        video_rect.top() + normalized_zone.top() * video_rect.height(),
        normalized_zone.width() * video_rect.width(),
        normalized_zone.height() * video_rect.height(),
    };
}

QPointF DetectionOverlay::normalizedVideoPoint(const QPointF& widget_point) const {
    const auto video_rect = displayedVideoRect();
    if (video_rect.isEmpty()) {
        return {};
    }
    return {
        std::clamp((widget_point.x() - video_rect.left()) / video_rect.width(), 0.0, 1.0),
        std::clamp((widget_point.y() - video_rect.top()) / video_rect.height(), 0.0, 1.0),
    };
}

DetectionOverlay::DragHandle DetectionOverlay::hitHandle(const QRectF& display_zone, const QPointF& point) const {
    const std::array handles{
        std::pair{ display_zone.topLeft(), DragHandle::TopLeft },
        std::pair{ QPointF(display_zone.center().x(), display_zone.top()), DragHandle::Top },
        std::pair{ display_zone.topRight(), DragHandle::TopRight },
        std::pair{ QPointF(display_zone.right(), display_zone.center().y()), DragHandle::Right },
        std::pair{ display_zone.bottomRight(), DragHandle::BottomRight },
        std::pair{ QPointF(display_zone.center().x(), display_zone.bottom()), DragHandle::Bottom },
        std::pair{ display_zone.bottomLeft(), DragHandle::BottomLeft },
        std::pair{ QPointF(display_zone.left(), display_zone.center().y()), DragHandle::Left },
    };
    for (const auto& [position, handle] : handles) {
        if (QRectF(position.x() - kDangerZoneHandleRadius, position.y() - kDangerZoneHandleRadius,
                   kDangerZoneHandleRadius * 2.0, kDangerZoneHandleRadius * 2.0)
                .contains(point)) {
            return handle;
        }
    }
    return DragHandle::None;
}

void DetectionOverlay::evaluateDangerZoneOccupancy() {
    if (!has_frame_ || (danger_zones_.isEmpty() && aruco_markers_.isEmpty())) {
        occupied_danger_zones_.clear();
        occupied_aruco_markers_.clear();
        emit dangerZoneOccupancyEvaluated(false, -1, {}, 0.0);
        return;
    }
    occupied_danger_zones_.clear();
    occupied_aruco_markers_.clear();
    QString first_class_name;
    double first_confidence = 0.0;
    const QRectF normalized_video(0.0, 0.0, 1.0, 1.0);
    for (const auto& detection : frame_.detections) {
        const auto class_name = detection.class_name.trimmed().toUpper();
        if (!person_classes_.contains(class_name) ||
            (detection.likelihood > 0.0 && detection.likelihood < minimum_person_confidence_)) {
            continue;
        }
        const auto box = MapOnvifBoundingBox(frame_, detection.bounding_box, normalized_video);
        for (qsizetype zone_index = 0; zone_index < danger_zones_.size(); ++zone_index) {
            if (danger_zones_[zone_index].intersects(box)) {
                if (occupied_danger_zones_.isEmpty() && occupied_aruco_markers_.isEmpty()) {
                    first_class_name = detection.class_name;
                    first_confidence = detection.likelihood;
                }
                occupied_danger_zones_.insert(static_cast<int>(zone_index));
            }
        }
        for (auto marker = aruco_markers_.cbegin(); marker != aruco_markers_.cend(); ++marker) {
            if (marker->danger_rect.intersects(box)) {
                if (occupied_danger_zones_.isEmpty() && occupied_aruco_markers_.isEmpty()) {
                    first_class_name = detection.class_name;
                    first_confidence = detection.likelihood;
                }
                occupied_aruco_markers_.insert(marker.key());
            }
        }
    }
    if (occupied_danger_zones_.isEmpty() && occupied_aruco_markers_.isEmpty()) {
        emit dangerZoneOccupancyEvaluated(false, -1, {}, 0.0);
    } else {
        const int zone_index = occupied_danger_zones_.isEmpty() ? -*occupied_aruco_markers_.constBegin() - 1
                                                                : *occupied_danger_zones_.constBegin();
        emit dangerZoneOccupancyEvaluated(true, zone_index, first_class_name, first_confidence);
    }
}

void DetectionOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Qt::black);

    const auto video_rect = displayedVideoRect();
    if (video_frame_.isValid()) {
        video_frame_.paint(&painter, video_rect, {});
    }

    const auto& visible_zones = danger_zone_editing_ ? draft_danger_zones_ : danger_zones_;
    if (danger_zones_visible_ || danger_zone_editing_) {
        painter.save();
        painter.setClipRect(video_rect);
        for (qsizetype index = 0; index < visible_zones.size(); ++index) {
            const auto display_zone = dangerZoneDisplayRect(visible_zones[index]);
            const bool selected = danger_zone_editing_ && index == selected_danger_zone_;
            const bool occupied = occupied_danger_zones_.contains(static_cast<int>(index));
            const auto color = occupied ? QColor(255, 59, 48) : QColor(255, 196, 64);
            painter.setPen(QPen(color, selected ? 3 : 2));
            painter.setBrush(QColor(color.red(), color.green(), color.blue(), occupied ? 55 : 45));
            painter.drawRect(display_zone);
            if (selected) {
                const std::array positions{
                    display_zone.topLeft(),     QPointF(display_zone.center().x(), display_zone.top()),
                    display_zone.topRight(),    QPointF(display_zone.right(), display_zone.center().y()),
                    display_zone.bottomRight(), QPointF(display_zone.center().x(), display_zone.bottom()),
                    display_zone.bottomLeft(),  QPointF(display_zone.left(), display_zone.center().y()),
                };
                painter.setPen(QPen(QColor(30, 30, 30), 1));
                painter.setBrush(QColor(255, 196, 64));
                for (const auto& position : positions) {
                    painter.drawRect(QRectF(position.x() - 4.0, position.y() - 4.0, 8.0, 8.0));
                }
            }
        }
        for (auto marker = aruco_markers_.cbegin(); marker != aruco_markers_.cend(); ++marker) {
            const bool occupied = occupied_aruco_markers_.contains(marker.key());
            const auto color = occupied ? QColor(255, 59, 48) : QColor(255, 196, 64);
            painter.setPen(QPen(color, 2));
            painter.setBrush(QColor(color.red(), color.green(), color.blue(), occupied ? 55 : 45));
            painter.drawRect(dangerZoneDisplayRect(marker->danger_rect));

            QPolygonF display_corners;
            for (const auto& corner : marker->corners) {
                display_corners.append(QPointF(video_rect.left() + corner.x() * video_rect.width(),
                                               video_rect.top() + corner.y() * video_rect.height()));
            }
            painter.setPen(QPen(QColor(78, 201, 176), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(display_corners);
            painter.drawText(display_corners.boundingRect().topLeft() + QPointF(4, -4),
                             QStringLiteral("ArUco %1").arg(marker.key()));
        }
        painter.restore();
    }

    if (!channel_label_.isEmpty()) {
        painter.save();
        constexpr double kReferenceVideoWidth = 704.0;
        const auto badge_scale = std::clamp(width() / kReferenceVideoWidth, 0.86, 1.12);
        const auto scaled = [badge_scale](const int value) { return qRound(value * badge_scale); };
        QFont badge_font = painter.font();
        badge_font.setBold(true);
        if (badge_font.pointSizeF() > 0.0) {
            badge_font.setPointSizeF(badge_font.pointSizeF() * badge_scale);
        } else if (badge_font.pixelSize() > 0) {
            badge_font.setPixelSize(scaled(badge_font.pixelSize()));
        }
        painter.setFont(badge_font);
        const QFontMetrics metrics(badge_font);
        const auto badge_width = metrics.horizontalAdvance(channel_label_) + scaled(20);
        const QRect badge_rect(scaled(10), scaled(10), badge_width, scaled(26));
        painter.setPen(QPen(QColor(60, 60, 60), 1));
        painter.setBrush(QColor(24, 24, 24, 220));
        painter.drawRoundedRect(badge_rect, scaled(5), scaled(5));
        painter.setPen(QColor(181, 206, 168));
        painter.drawText(badge_rect, Qt::AlignCenter, channel_label_);
        painter.restore();
    }

    if (metadata_connected_) {
        const auto status =
            has_frame_ ? QStringLiteral("ONVIF · %1").arg(frame_.detections.size()) : QStringLiteral("ONVIF · LIVE");
        const QFontMetrics metrics(font());
        const auto width = metrics.horizontalAdvance(status) + 18;
        const QRect status_rect(this->width() - width - 10, 10, width, 24);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(24, 24, 24, 210));
        painter.drawRoundedRect(status_rect, 4, 4);
        painter.setPen(QColor(78, 201, 176));
        painter.drawText(status_rect, Qt::AlignCenter, status);
    }

    if (!has_frame_ || !video_frame_.isValid()) {
        return;
    }

    painter.setClipRect(video_rect);
    QPen box_pen(QColor(78, 201, 176));
    box_pen.setWidth(2);

    for (const auto& detection : frame_.detections) {
        const auto box = MapOnvifBoundingBox(frame_, detection.bounding_box, video_rect);
        painter.setPen(box_pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);

        auto label = detection.class_name.isEmpty() ? QStringLiteral("Object") : detection.class_name;
        if (detection.likelihood > 0.0) {
            label += QStringLiteral(" %1%").arg(qRound(detection.likelihood * 100.0));
        }
        if (!detection.object_id.isEmpty()) {
            label += QStringLiteral("  #%1").arg(detection.object_id);
        }

        const QFontMetrics metrics(painter.font());
        const auto label_width = metrics.horizontalAdvance(label) + 12;
        const auto label_height = metrics.height() + 6;
        const auto label_y = std::max(video_rect.top(), box.top() - label_height);
        QRectF label_rect(box.left(), label_y, label_width, label_height);
        label_rect = label_rect.intersected(video_rect);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(20, 96, 79, 225));
        painter.drawRoundedRect(label_rect, 3, 3);
        painter.setPen(Qt::white);
        painter.drawText(label_rect.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    }
}

void DetectionOverlay::mousePressEvent(QMouseEvent* event) {
    if (!danger_zone_editing_ || event->button() != Qt::LeftButton ||
        !displayedVideoRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    drag_handle_ = DragHandle::None;
    if (selected_danger_zone_ >= 0 && selected_danger_zone_ < draft_danger_zones_.size()) {
        drag_handle_ = hitHandle(dangerZoneDisplayRect(draft_danger_zones_[selected_danger_zone_]), event->position());
    }
    if (drag_handle_ == DragHandle::None) {
        selected_danger_zone_ = -1;
        for (qsizetype index = draft_danger_zones_.size(); index-- > 0;) {
            if (dangerZoneDisplayRect(draft_danger_zones_[index]).contains(event->position())) {
                selected_danger_zone_ = static_cast<int>(index);
                drag_handle_ = DragHandle::Move;
                break;
            }
        }
    }
    if (selected_danger_zone_ >= 0) {
        drag_start_point_ = normalizedVideoPoint(event->position());
        drag_start_zone_ = draft_danger_zones_[selected_danger_zone_];
        event->accept();
    }
    update();
}

void DetectionOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (!danger_zone_editing_ || drag_handle_ == DragHandle::None || selected_danger_zone_ < 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    updateEditedDangerZone(normalizedVideoPoint(event->position()));
    event->accept();
}

void DetectionOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && drag_handle_ != DragHandle::None) {
        updateEditedDangerZone(normalizedVideoPoint(event->position()));
        drag_handle_ = DragHandle::None;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DetectionOverlay::keyPressEvent(QKeyEvent* event) {
    if (danger_zone_editing_ && event->key() == Qt::Key_Delete) {
        deleteSelectedDangerZone();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void DetectionOverlay::updateEditedDangerZone(const QPointF& normalized_point) {
    if (selected_danger_zone_ < 0 || selected_danger_zone_ >= draft_danger_zones_.size()) {
        return;
    }
    QRectF zone = drag_start_zone_;
    if (drag_handle_ == DragHandle::Move) {
        const auto delta = normalized_point - drag_start_point_;
        zone.moveLeft(std::clamp(drag_start_zone_.left() + delta.x(), 0.0, 1.0 - zone.width()));
        zone.moveTop(std::clamp(drag_start_zone_.top() + delta.y(), 0.0, 1.0 - zone.height()));
    } else {
        if (drag_handle_ == DragHandle::TopLeft || drag_handle_ == DragHandle::Left ||
            drag_handle_ == DragHandle::BottomLeft) {
            zone.setLeft(std::clamp(normalized_point.x(), 0.0, zone.right() - kMinimumDangerZoneSize));
        }
        if (drag_handle_ == DragHandle::TopRight || drag_handle_ == DragHandle::Right ||
            drag_handle_ == DragHandle::BottomRight) {
            zone.setRight(std::clamp(normalized_point.x(), zone.left() + kMinimumDangerZoneSize, 1.0));
        }
        if (drag_handle_ == DragHandle::TopLeft || drag_handle_ == DragHandle::Top ||
            drag_handle_ == DragHandle::TopRight) {
            zone.setTop(std::clamp(normalized_point.y(), 0.0, zone.bottom() - kMinimumDangerZoneSize));
        }
        if (drag_handle_ == DragHandle::BottomLeft || drag_handle_ == DragHandle::Bottom ||
            drag_handle_ == DragHandle::BottomRight) {
            zone.setBottom(std::clamp(normalized_point.y(), zone.top() + kMinimumDangerZoneSize, 1.0));
        }
    }
    draft_danger_zones_[selected_danger_zone_] = zone;
    update();
}

}  // namespace logistics::control_center
