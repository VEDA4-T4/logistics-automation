#pragma once

#include <QSize>
#include <QWidget>

#include "logistics/control_center/onvif_metadata.hpp"

class QTimer;

namespace logistics::control_center {

class DetectionOverlay final : public QWidget {
public:
    explicit DetectionOverlay(QWidget* parent = nullptr);

    void setDetectionFrame(const OnvifDetectionFrame& frame);
    void setVideoSize(const QSize& size);
    void setMetadataState(bool connected, const QString& detail = {});
    void setStaleTimeout(int timeout_ms);
    void clearDetections();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QRectF displayedVideoRect() const;

    OnvifDetectionFrame frame_;
    QSize video_size_;
    QTimer* stale_timer_{ nullptr };
    QString metadata_detail_;
    bool has_frame_{ false };
    bool metadata_connected_{ false };
};

}  // namespace logistics::control_center
