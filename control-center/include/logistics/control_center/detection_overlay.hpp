#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QVideoFrame>
#include <QWidget>

#include "logistics/control_center/aruco_detector.hpp"
#include "logistics/control_center/onvif_metadata.hpp"

class QTimer;
class QThread;
class QVideoSink;
class QKeyEvent;
class QMouseEvent;

namespace logistics::control_center {

class DetectionOverlay final : public QWidget {
    Q_OBJECT

public:
    explicit DetectionOverlay(QWidget* parent = nullptr);
    ~DetectionOverlay() override;

    void setChannelLabel(const QString& label);
    void setDetectionFrame(const OnvifDetectionFrame& frame);
    void setMetadataState(bool connected, const QString& detail = {});
    void setStaleTimeout(int timeout_ms);
    void clearDetections();
    [[nodiscard]] QVideoSink* videoSink() const;
    void setDangerZones(QList<QRectF> zones);
    [[nodiscard]] const QList<QRectF>& dangerZones() const noexcept;
    void setDangerZonesVisible(bool visible);
    [[nodiscard]] bool dangerZonesVisible() const noexcept;
    void setPersonClasses(QStringList classes);
    void setMinimumPersonConfidence(double confidence);
    void beginDangerZoneEditing();
    void commitDangerZoneEditing();
    void cancelDangerZoneEditing();
    [[nodiscard]] bool isDangerZoneEditing() const noexcept;
    void addDangerZone();
    void deleteSelectedDangerZone();
    void setArucoMarkers(const QList<ArucoMarkerRegion>& markers);
    void clearArucoMarkers();

signals:
    void dangerZoneOccupancyEvaluated(bool occupied, int zone_index, const QString& class_name, double confidence);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class DragHandle {
        None,
        Move,
        TopLeft,
        Top,
        TopRight,
        Right,
        BottomRight,
        Bottom,
        BottomLeft,
        Left,
    };

    [[nodiscard]] QRectF displayedVideoRect() const;
    [[nodiscard]] QRectF dangerZoneDisplayRect(const QRectF& normalized_zone) const;
    [[nodiscard]] QPointF normalizedVideoPoint(const QPointF& widget_point) const;
    [[nodiscard]] DragHandle hitHandle(const QRectF& display_zone, const QPointF& point) const;
    void evaluateDangerZoneOccupancy();
    void updateEditedDangerZone(const QPointF& normalized_point);
    void queueArucoDetection(QImage image);

    OnvifDetectionFrame frame_;
    QVideoFrame video_frame_;
    QSize video_size_;
    QVideoSink* video_sink_{ nullptr };
    QTimer* stale_timer_{ nullptr };
    QThread* aruco_thread_{ nullptr };
    ArucoDetectorWorker* aruco_worker_{ nullptr };
    QString channel_label_;
    QString metadata_detail_;
    QList<QRectF> danger_zones_;
    QList<QRectF> draft_danger_zones_;
    QHash<int, ArucoMarkerRegion> aruco_markers_;
    QStringList person_classes_{ QStringLiteral("HEAD"), QStringLiteral("FACE"), QStringLiteral("HUMAN") };
    QPointF drag_start_point_;
    QRectF drag_start_zone_;
    DragHandle drag_handle_{ DragHandle::None };
    int selected_danger_zone_{ -1 };
    QSet<int> occupied_danger_zones_;
    QSet<int> occupied_aruco_markers_;
    quint64 aruco_frame_counter_{ 0 };
    double minimum_person_confidence_{ 0.5 };
    bool has_frame_{ false };
    bool metadata_connected_{ false };
    bool danger_zones_visible_{ true };
    bool danger_zone_editing_{ false };
    bool aruco_detection_in_flight_{ false };
};

}  // namespace logistics::control_center
