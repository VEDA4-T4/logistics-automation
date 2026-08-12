#include "logistics/control_center/detection_overlay.hpp"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <cmath>

#include "logistics/control_center/aruco_detector.hpp"

namespace {

bool isNear(const QColor& color, const QColor& expected, int tolerance = 3) {
    return std::abs(color.red() - expected.red()) <= tolerance &&
           std::abs(color.green() - expected.green()) <= tolerance &&
           std::abs(color.blue() - expected.blue()) <= tolerance;
}

bool hasColorNear(const QImage& image, const QPoint& point, const QColor& expected) {
    for (int y = point.y() - 3; y <= point.y() + 3; ++y) {
        for (int x = point.x() - 3; x <= point.x() + 3; ++x) {
            if (isNear(image.pixelColor(x, y), expected, 10)) {
                return true;
            }
        }
    }
    return false;
}

QImage makeMarker(const quint16 code) {
    QImage image(320, 320, QImage::Format_Grayscale8);
    image.fill(Qt::white);
    constexpr int cell_size = 40;
    constexpr int offset = 40;
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 6; ++column) {
            const bool border = row == 0 || row == 5 || column == 0 || column == 5;
            const int bit_index = (row - 1) * 4 + column - 1;
            const bool black = border || (!border && ((code >> (15 - bit_index)) & 1U) == 0);
            if (!black) {
                continue;
            }
            for (int y = offset + row * cell_size; y < offset + (row + 1) * cell_size; ++y) {
                auto* line = image.scanLine(y);
                std::fill(line + offset + column * cell_size, line + offset + (column + 1) * cell_size, 0);
            }
        }
    }
    return image;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef Q_OS_WIN
    qputenv("QT_QPA_PLATFORM", "windows");
#endif
    QApplication app(argc, argv);
    const auto markers = logistics::control_center::DetectAruco4x4Markers(makeMarker(0xB532));
    if (markers.size() != 1 || markers.front().marker_id != 0) {
        return 1;
    }
    const auto marker_10 = logistics::control_center::DetectAruco4x4Markers(makeMarker(0xF991));
    if (marker_10.size() != 1 || marker_10.front().marker_id != 10) {
        return 2;
    }
    QImage rotated_marker(460, 460, QImage::Format_Grayscale8);
    rotated_marker.fill(Qt::white);
    {
        QPainter painter(&rotated_marker);
        painter.translate(rotated_marker.rect().center());
        painter.rotate(25);
        painter.drawImage(QPoint(-160, -160), makeMarker(0xB532));
    }
    const auto rotated_markers = logistics::control_center::DetectAruco4x4Markers(rotated_marker);
    if (rotated_markers.size() != 1 || rotated_markers.front().marker_id != 0) {
        return 3;
    }
    logistics::control_center::DetectionOverlay widget;
    widget.resize(640, 360);

    logistics::control_center::OnvifDetectionFrame frame;
    frame.translate = { -1.0, 1.0 };
    frame.scale = { 2.0 / 1280.0, -2.0 / 720.0 };
    logistics::control_center::OnvifDetection detection;
    detection.object_id = QStringLiteral("62");
    detection.bounding_box = QRectF(QPointF(100, 100), QPointF(300, 300));
    detection.class_name = QStringLiteral("Vehicle");
    detection.likelihood = 0.41;
    frame.detections.append(detection);
    widget.setMetadataState(true);
    widget.setDetectionFrame(frame);

    const QColor box_color(78, 201, 176);
    QImage before_video(widget.size(), QImage::Format_ARGB32_Premultiplied);
    before_video.fill(Qt::transparent);
    widget.render(&before_video);
    for (int y = 47; y <= 53; ++y) {
        for (int x = 47; x <= 53; ++x) {
            if (isNear(before_video.pixelColor(x, y), box_color, 10)) {
                return 1;
            }
        }
    }

    const QColor video_color(12, 24, 36);
    QImage video_image(1280, 720, QImage::Format_ARGB32_Premultiplied);
    video_image.fill(video_color);
    widget.videoSink()->setVideoFrame(QVideoFrame(video_image));

    QImage rendered(widget.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    widget.render(&rendered);

    if (!isNear(rendered.pixelColor(320, 180), video_color)) {
        return 4;
    }

    bool found_box_pixel = false;
    for (int y = 47; y <= 53 && !found_box_pixel; ++y) {
        for (int x = 47; x <= 53; ++x) {
            if (isNear(rendered.pixelColor(x, y), box_color, 10)) {
                found_box_pixel = true;
                break;
            }
        }
    }
    if (!found_box_pixel) {
        return 5;
    }

    bool occupied = false;
    QObject::connect(&widget, &logistics::control_center::DetectionOverlay::dangerZoneOccupancyEvaluated,
                     [&occupied](bool value, int, const QString&, double) { occupied = value; });
    widget.setPersonClasses({ QStringLiteral("Vehicle") });
    widget.setDangerZones({ QRectF(0.2, 0.3, 0.3, 0.2) });
    rendered.fill(Qt::transparent);
    widget.render(&rendered);
    if (!hasColorNear(rendered, QPoint(300, 108), QColor(255, 196, 64))) {
        return 3;
    }
    detection.class_name = QStringLiteral("Face");
    detection.likelihood = 0.9;
    detection.bounding_box = QRectF(QPointF(128, 72), QPointF(320, 252));
    frame.detections = { detection };
    widget.setDetectionFrame(frame);
    if (!occupied) {
        return 4;
    }
    rendered.fill(Qt::transparent);
    widget.render(&rendered);
    if (!hasColorNear(rendered, QPoint(300, 108), QColor(255, 59, 48))) {
        return 5;
    }
    widget.setDangerZones({ QRectF(0.2, 0.3, 0.3, 0.2), QRectF(0.05, 0.05, 0.2, 0.2) });
    rendered.fill(Qt::transparent);
    widget.render(&rendered);
    if (!hasColorNear(rendered, QPoint(300, 108), QColor(255, 59, 48)) ||
        !hasColorNear(rendered, QPoint(100, 90), QColor(255, 59, 48))) {
        return 6;
    }
    widget.setDangerZones({ QRectF(0.2, 0.3, 0.3, 0.2) });
    for (const auto& class_name : { QStringLiteral("Head"), QStringLiteral("Human") }) {
        occupied = false;
        frame.detections.front().class_name = class_name;
        widget.setDetectionFrame(frame);
        if (!occupied) {
            return 7;
        }
    }
    widget.setDangerZones({});
    widget.setArucoMarkers(markers);
    occupied = false;
    widget.setDetectionFrame(frame);
    rendered.fill(Qt::transparent);
    widget.render(&rendered);
    if (!occupied || !hasColorNear(rendered, QPoint(320, 333), QColor(255, 59, 48))) {
        return 8;
    }
    widget.setArucoMarkers({});
    occupied = true;
    widget.setDetectionFrame(frame);
    if (occupied) {
        return 9;
    }
    widget.setDangerZones({ QRectF(0.2, 0.3, 0.3, 0.2) });
    widget.setDangerZonesVisible(false);
    occupied = false;
    widget.setDetectionFrame(frame);
    if (!occupied) {
        return 10;
    }

    widget.beginDangerZoneEditing();
    const auto drag = [&widget](const QPointF& from, const QPointF& to) {
        QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &press);
        QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &release);
    };
    drag(QPointF(224, 144), QPointF(288, 144));
    widget.commitDangerZoneEditing();
    if (widget.dangerZones().size() != 1 || std::abs(widget.dangerZones().front().x() - 0.3) > 0.001) {
        return 11;
    }

    widget.beginDangerZoneEditing();
    drag(QPointF(384, 180), QPointF(448, 216));
    widget.commitDangerZoneEditing();
    if (std::abs(widget.dangerZones().front().width() - 0.4) > 0.001 ||
        std::abs(widget.dangerZones().front().height() - 0.3) > 0.001) {
        return 12;
    }

    widget.beginDangerZoneEditing();
    QKeyEvent delete_key(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(&widget, &delete_key);
    widget.commitDangerZoneEditing();
    return widget.dangerZones().isEmpty() ? 0 : 13;
}
