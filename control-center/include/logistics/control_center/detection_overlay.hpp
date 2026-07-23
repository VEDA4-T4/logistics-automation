#pragma once

#include <QSize>
#include <QVideoFrame>
#include <QWidget>
#include <deque>

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
    void setSynchronizationDelay(int delay_ms);
    void setStaleTimeout(int timeout_ms);
    void clearDetections();
    [[nodiscard]] QVideoSink* videoSink() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct ScheduledFrame {
        qint64 due_msecs_since_epoch{ 0 };
        OnvifDetectionFrame frame;
    };

    [[nodiscard]] QRectF displayedVideoRect() const;
    void presentDetectionFrame(const OnvifDetectionFrame& frame);
    void presentDueDetectionFrames();
    void scheduleNextDetectionFrame();
    void clearVisibleDetections();

    OnvifDetectionFrame frame_;
    QVideoFrame video_frame_;
    QSize video_size_;
    QVideoSink* video_sink_{ nullptr };
    QTimer* stale_timer_{ nullptr };
    QTimer* synchronization_timer_{ nullptr };
    std::deque<ScheduledFrame> scheduled_frames_;
    QString channel_label_;
    QString metadata_detail_;
    int synchronization_delay_ms_{ 0 };
    bool has_frame_{ false };
    bool metadata_connected_{ false };
};

}  // namespace logistics::control_center
