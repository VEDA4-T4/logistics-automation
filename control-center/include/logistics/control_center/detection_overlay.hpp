#pragma once

#include <QSize>
#include <QVideoFrame>
#include <QWidget>

#include "logistics/control_center/onvif_metadata.hpp"

class QTimer;
class QVideoSink;

namespace logistics::control_center {

class DetectionOverlay final : public QWidget {
public:
    explicit DetectionOverlay(QWidget* parent = nullptr);

    void setChannelLabel(const QString& label);
    void setDetectionFrame(const OnvifDetectionFrame& frame);
    void setMetadataState(bool connected, const QString& detail = {});
    void setStaleTimeout(int timeout_ms);
    void clearDetections();
    [[nodiscard]] QVideoSink* videoSink() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QRectF displayedVideoRect() const;

    OnvifDetectionFrame frame_;
    QVideoFrame video_frame_;
    QSize video_size_;
    QVideoSink* video_sink_{ nullptr };
    QTimer* stale_timer_{ nullptr };
    QString channel_label_;
    QString metadata_detail_;
    bool has_frame_{ false };
    bool metadata_connected_{ false };
};

}  // namespace logistics::control_center
