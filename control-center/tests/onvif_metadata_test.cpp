#include "logistics/control_center/onvif_metadata.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <cmath>

namespace {

bool almostEqual(double left, double right) {
    return std::abs(left - right) < 0.01;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QByteArray xml = R"xml(
        <tt:MetadataStream xmlns:tt="http://www.onvif.org/ver10/schema">
          <tt:VideoAnalytics>
            <tt:Frame UtcTime="2026-07-08T00:32:39.591Z">
              <tt:Transformation>
                <tt:Translate x="-1.0" y="1.0"/>
                <tt:Scale x="0.000772" y="-0.001316"/>
              </tt:Transformation>
              <tt:Object ObjectId="62">
                <tt:Appearance>
                  <tt:Shape>
                    <tt:BoundingBox left="891.0" top="196.0" right="1361.0" bottom="716.0"/>
                    <tt:CenterOfGravity x="1126.0" y="456.0"/>
                  </tt:Shape>
                  <tt:Class>
                    <tt:ClassCandidate>
                      <tt:Type>Face</tt:Type>
                      <tt:Likelihood>0.41</tt:Likelihood>
                    </tt:ClassCandidate>
                    <tt:Type Likelihood="0.41">Face</tt:Type>
                  </tt:Class>
                </tt:Appearance>
              </tt:Object>
            </tt:Frame>
          </tt:VideoAnalytics>
        </tt:MetadataStream>
    )xml";

    const auto result = logistics::control_center::ParseOnvifMetadata(xml);
    if (!result.isValid() || result.frames.size() != 1) {
        qCritical() << "parse failed" << result.error;
        return 1;
    }
    const auto& frame = result.frames.front();
    if (frame.utc_time.toUTC().toString(Qt::ISODateWithMs) != QStringLiteral("2026-07-08T00:32:39.591Z") ||
        !almostEqual(frame.translate.x(), -1.0) || !almostEqual(frame.translate.y(), 1.0) ||
        frame.detections.size() != 1) {
        qCritical() << "frame fields differ";
        return 1;
    }
    const auto& detection = frame.detections.front();
    if (detection.object_id != QStringLiteral("62") || detection.class_name != QStringLiteral("Face") ||
        !almostEqual(detection.likelihood, 0.41) || !almostEqual(detection.bounding_box.left(), 891.0) ||
        !almostEqual(detection.bounding_box.bottom(), 716.0)) {
        qCritical() << "detection fields differ";
        return 1;
    }

    const auto mapped =
        logistics::control_center::MapOnvifBoundingBox(frame, detection.bounding_box, QRectF(0, 0, 1000, 500));
    if (!almostEqual(mapped.left(), 343.93) || !almostEqual(mapped.top(), 64.48) ||
        !almostEqual(mapped.right(), 525.35) || !almostEqual(mapped.bottom(), 235.57)) {
        qCritical() << "mapped rectangle differs" << mapped;
        return 1;
    }

    const QByteArray event_xml =
        "<tt:MetadataStream xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
        "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
        "<tt:Event><wsnt:NotificationMessage/></tt:Event></tt:MetadataStream>";
    const auto event_result = logistics::control_center::ParseOnvifMetadata(event_xml);
    if (!event_result.isValid() || !event_result.frames.isEmpty()) {
        qCritical() << "valid event metadata was treated as an error";
        return 1;
    }
    return 0;
}
