#include "logistics/control_center/onvif_metadata.hpp"

#include <QXmlStreamReader>
#include <algorithm>

namespace logistics::control_center {
namespace {

double attributeAsDouble(const QXmlStreamAttributes& attributes, QLatin1StringView name, double fallback = 0.0) {
    bool valid = false;
    const auto value = attributes.value(name).toDouble(&valid);
    return valid ? value : fallback;
}

QPointF mapMetadataPoint(const OnvifDetectionFrame& frame, const QPointF& point, const QRectF& displayed_video_rect) {
    const auto normalized_x = frame.translate.x() + frame.scale.x() * point.x();
    const auto normalized_y = frame.translate.y() + frame.scale.y() * point.y();
    return {
        displayed_video_rect.left() + ((normalized_x + 1.0) / 2.0) * displayed_video_rect.width(),
        displayed_video_rect.top() + ((1.0 - normalized_y) / 2.0) * displayed_video_rect.height(),
    };
}

}  // namespace

OnvifMetadataParseResult ParseOnvifMetadata(const QByteArray& xml) {
    OnvifMetadataParseResult result;
    QXmlStreamReader reader(xml);
    OnvifDetectionFrame frame;
    OnvifDetection detection;
    QString candidate_class;
    double candidate_likelihood = 0.0;
    bool in_frame = false;
    bool in_object = false;
    bool in_class = false;
    bool in_class_candidate = false;
    bool has_bounding_box = false;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name();
            const auto attributes = reader.attributes();

            if (name == QLatin1StringView("Frame")) {
                frame = {};
                const auto utc_time = attributes.value(QLatin1StringView("UtcTime")).toString();
                frame.utc_time = QDateTime::fromString(utc_time, Qt::ISODateWithMs);
                if (!frame.utc_time.isValid()) {
                    frame.utc_time = QDateTime::fromString(utc_time, Qt::ISODate);
                }
                in_frame = true;
            } else if (in_frame && name == QLatin1StringView("Translate")) {
                frame.translate = { attributeAsDouble(attributes, QLatin1StringView("x")),
                                    attributeAsDouble(attributes, QLatin1StringView("y")) };
            } else if (in_frame && name == QLatin1StringView("Scale")) {
                frame.scale = { attributeAsDouble(attributes, QLatin1StringView("x"), 1.0),
                                attributeAsDouble(attributes, QLatin1StringView("y"), 1.0) };
            } else if (in_frame && name == QLatin1StringView("Object")) {
                detection = {};
                detection.object_id = attributes.value(QLatin1StringView("ObjectId")).toString();
                has_bounding_box = false;
                in_object = true;
            } else if (in_object && name == QLatin1StringView("BoundingBox")) {
                const auto left = attributeAsDouble(attributes, QLatin1StringView("left"));
                const auto top = attributeAsDouble(attributes, QLatin1StringView("top"));
                const auto right = attributeAsDouble(attributes, QLatin1StringView("right"));
                const auto bottom = attributeAsDouble(attributes, QLatin1StringView("bottom"));
                detection.bounding_box = QRectF(QPointF(left, top), QPointF(right, bottom)).normalized();
                has_bounding_box = detection.bounding_box.isValid() && !detection.bounding_box.isEmpty();
            } else if (in_object && name == QLatin1StringView("CenterOfGravity")) {
                detection.center_of_gravity = { attributeAsDouble(attributes, QLatin1StringView("x")),
                                                attributeAsDouble(attributes, QLatin1StringView("y")) };
            } else if (in_object && name == QLatin1StringView("Class")) {
                in_class = true;
            } else if (in_class && name == QLatin1StringView("ClassCandidate")) {
                candidate_class.clear();
                candidate_likelihood = 0.0;
                in_class_candidate = true;
            } else if (in_class && name == QLatin1StringView("Type")) {
                const auto likelihood = attributeAsDouble(attributes, QLatin1StringView("Likelihood"), -1.0);
                const auto type = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                if (in_class_candidate) {
                    candidate_class = type;
                    if (likelihood >= 0.0) {
                        candidate_likelihood = likelihood;
                    }
                } else if (!type.isEmpty() && (detection.class_name.isEmpty() || likelihood >= 0.0)) {
                    detection.class_name = type;
                    if (likelihood >= 0.0) {
                        detection.likelihood = likelihood;
                    }
                }
            } else if (in_class_candidate && name == QLatin1StringView("Likelihood")) {
                bool valid = false;
                const auto likelihood =
                    reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed().toDouble(&valid);
                if (valid) {
                    candidate_likelihood = likelihood;
                }
            }
        } else if (reader.isEndElement()) {
            const auto name = reader.name();
            if (name == QLatin1StringView("ClassCandidate")) {
                if (!candidate_class.isEmpty() &&
                    (detection.class_name.isEmpty() || candidate_likelihood >= detection.likelihood)) {
                    detection.class_name = candidate_class;
                    detection.likelihood = candidate_likelihood;
                }
                in_class_candidate = false;
            } else if (name == QLatin1StringView("Class")) {
                in_class = false;
            } else if (name == QLatin1StringView("Object")) {
                if (has_bounding_box) {
                    frame.detections.append(detection);
                }
                in_object = false;
            } else if (name == QLatin1StringView("Frame")) {
                result.frames.append(frame);
                in_frame = false;
            }
        }
    }

    if (reader.hasError()) {
        result.frames.clear();
        result.error = QStringLiteral("ONVIF XML 파싱 오류: %1").arg(reader.errorString());
    }
    return result;
}

QRectF MapOnvifBoundingBox(const OnvifDetectionFrame& frame, const QRectF& bounding_box,
                           const QRectF& displayed_video_rect) {
    const auto top_left = mapMetadataPoint(frame, bounding_box.topLeft(), displayed_video_rect);
    const auto bottom_right = mapMetadataPoint(frame, bounding_box.bottomRight(), displayed_video_rect);
    return QRectF(top_left, bottom_right).normalized();
}

}  // namespace logistics::control_center
