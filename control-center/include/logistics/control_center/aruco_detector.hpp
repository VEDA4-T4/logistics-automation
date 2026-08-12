#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPolygonF>
#include <QRectF>

namespace logistics::control_center {

struct ArucoMarkerRegion {
    int marker_id{ -1 };
    QPolygonF corners;
    QRectF danger_rect;
};

[[nodiscard]] QList<ArucoMarkerRegion> DetectAruco4x4Markers(const QImage& image, double danger_margin = 0.05);

class ArucoDetectorWorker final : public QObject {
    Q_OBJECT

public:
    explicit ArucoDetectorWorker(double danger_margin, QObject* parent = nullptr);

    void detect(const QImage& image);

signals:
    void markersDetected(const QList<ArucoMarkerRegion>& markers);

private:
    double danger_margin_{ 0.05 };
};

}  // namespace logistics::control_center

Q_DECLARE_METATYPE(logistics::control_center::ArucoMarkerRegion)
