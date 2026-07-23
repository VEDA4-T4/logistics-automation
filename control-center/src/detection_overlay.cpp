#include "logistics/control_center/detection_overlay.hpp"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>
#include <QVideoSink>
#include <algorithm>

namespace logistics::control_center {

DetectionOverlay::DetectionOverlay(QWidget* parent)
    : QWidget(parent), video_sink_(new QVideoSink(this)), stale_timer_(new QTimer(this)) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    stale_timer_->setSingleShot(true);
    stale_timer_->setInterval(1500);
    connect(stale_timer_, &QTimer::timeout, this, &DetectionOverlay::clearDetections);
    connect(video_sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
        video_frame_ = frame;
        video_size_ = frame.isValid() ? frame.size() : QSize();
        update();
    });
}

void DetectionOverlay::setDetectionFrame(const OnvifDetectionFrame& frame) {
    frame_ = frame;
    has_frame_ = true;
    stale_timer_->start();
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
    update();
}

QVideoSink* DetectionOverlay::videoSink() const {
    return video_sink_;
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

void DetectionOverlay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), Qt::black);

    const auto video_rect = displayedVideoRect();
    if (video_frame_.isValid()) {
        video_frame_.paint(&painter, video_rect, {});
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

    if (!has_frame_) {
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

}  // namespace logistics::control_center
