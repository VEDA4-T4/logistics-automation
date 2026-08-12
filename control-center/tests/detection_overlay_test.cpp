#include "logistics/control_center/detection_overlay.hpp"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QVideoFrame>
#include <QVideoSink>
#include <cmath>

namespace {

bool isNear(const QColor& color, const QColor& expected, int tolerance = 3) {
    return std::abs(color.red() - expected.red()) <= tolerance &&
           std::abs(color.green() - expected.green()) <= tolerance &&
           std::abs(color.blue() - expected.blue()) <= tolerance;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    logistics::control_center::DetectionOverlay widget;
    widget.resize(640, 360);

    logistics::control_center::OnvifDetectionFrame frame;
    frame.translate = { -1.0, 1.0 };
    frame.scale = { 2.0 / 1280.0, -2.0 / 720.0 };
    logistics::control_center::OnvifDetection detection;
    detection.object_id = QStringLiteral("62");
    detection.bounding_box = QRectF(QPointF(100, 100), QPointF(300, 300));
    detection.class_name = QStringLiteral("Face");
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
        return 2;
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
    return found_box_pixel ? 0 : 3;
}
