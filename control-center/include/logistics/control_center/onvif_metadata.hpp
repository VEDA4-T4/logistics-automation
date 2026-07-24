#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace logistics::control_center {

struct OnvifDetection {
    QString object_id;
    QRectF bounding_box;
    QPointF center_of_gravity;
    QString class_name;
    double likelihood{ 0.0 };
};

struct OnvifDetectionFrame {
    QDateTime utc_time;
    QPointF translate{ 0.0, 0.0 };
    QPointF scale{ 1.0, 1.0 };
    QList<OnvifDetection> detections;
};

struct OnvifMetadataParseResult {
    QList<OnvifDetectionFrame> frames;
    QString error;

    [[nodiscard]] bool isValid() const {
        return error.isEmpty();
    }
};

[[nodiscard]] OnvifMetadataParseResult ParseOnvifMetadata(const QByteArray& xml);
[[nodiscard]] QRectF MapOnvifBoundingBox(const OnvifDetectionFrame& frame, const QRectF& bounding_box,
                                         const QRectF& displayed_video_rect);

}  // namespace logistics::control_center
