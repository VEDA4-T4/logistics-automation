#include "logistics/control_center/current_product_state.hpp"

#include <QJsonValue>
#include <QStringList>
#include <string>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::control_center {
namespace {

namespace mqtt = logistics::contracts::mqtt;

bool IsProductMessage(mqtt::MessageType type) {
    switch (type) {
        case mqtt::MessageType::kWorkCreated:
        case mqtt::MessageType::kBarcodeDetected:
        case mqtt::MessageType::kProductImage:
        case mqtt::MessageType::kProductInfo:
        case mqtt::MessageType::kDestinationSet:
        case mqtt::MessageType::kWorkCompleted:
            return true;
        default:
            return false;
    }
}

QString StringValue(const QJsonObject& object, const char* key) {
    const auto value = object.value(QString::fromLatin1(key));
    return value.isString() ? value.toString().trimmed() : QString{};
}

void SetIfPresent(QString& target, const QJsonObject& object, const char* key) {
    const auto value = StringValue(object, key);
    if (!value.isEmpty()) {
        target = value;
    }
}

ProductProcessingResult ParseProcessingResult(const QString& value, ProductProcessingResult fallback) {
    const auto normalized = value.trimmed().toUpper();
    if (normalized == QStringLiteral("PROCESSING") || normalized == QStringLiteral("IN_PROGRESS")) {
        return ProductProcessingResult::Processing;
    }
    if (normalized == QStringLiteral("SUCCESS") || normalized == QStringLiteral("COMPLETED")) {
        return ProductProcessingResult::Success;
    }
    if (normalized == QStringLiteral("FAILED") || normalized == QStringLiteral("REJECTED")) {
        return ProductProcessingResult::Failed;
    }
    return fallback;
}

enum class RecognitionReport { Unspecified, Success, Failed, MissingData };

RecognitionReport ParseRecognitionReport(const QJsonObject& data) {
    const auto status = StringValue(data, "recognitionStatus").toUpper();
    if (status == QStringLiteral("SUCCESS") || status == QStringLiteral("RECOGNIZED")) {
        return RecognitionReport::Success;
    }
    if (status == QStringLiteral("FAILED") || status == QStringLiteral("NOT_FOUND")) {
        return RecognitionReport::Failed;
    }
    if (status == QStringLiteral("MISSING_DATA") || status == QStringLiteral("INCOMPLETE")) {
        return RecognitionReport::MissingData;
    }
    const auto recognized = data.value(QStringLiteral("recognized"));
    if (recognized.isBool()) {
        return recognized.toBool() ? RecognitionReport::Success : RecognitionReport::Failed;
    }
    return RecognitionReport::Unspecified;
}

QString MissingRequiredFields(const CurrentProduct& product) {
    QStringList missing;
    if (product.barcode.isEmpty()) {
        missing.append(QStringLiteral("바코드"));
    }
    if (product.product_id.isEmpty()) {
        missing.append(QStringLiteral("상품 ID"));
    }
    if (product.destination.isEmpty()) {
        missing.append(QStringLiteral("목적지"));
    }
    return missing.join(QStringLiteral(", "));
}

QDateTime ParseTimestamp(const QJsonObject& envelope) {
    const auto value = envelope.value(QString::fromLatin1(mqtt::kTimestampField)).toString();
    auto timestamp = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!timestamp.isValid()) {
        timestamp = QDateTime::fromString(value, Qt::ISODate);
    }
    return timestamp;
}

}  // namespace

ProductUpdateResult CurrentProductState::applyEnvelope(const QJsonObject& envelope) {
    const auto type_text = envelope.value(QString::fromLatin1(mqtt::kMessageTypeField)).toString();
    const auto type = mqtt::MessageTypeFromString(type_text.toStdString());
    if (!IsProductMessage(type)) {
        return {};
    }

    ProductUpdateResult result{ .handled = true, .applied = false, .switched_work = false, .error = {} };
    const auto message_id = envelope.value(QString::fromLatin1(mqtt::kMessageIdField)).toString().trimmed();
    const auto source_id = envelope.value(QString::fromLatin1(mqtt::kSourceIdField)).toString().trimmed();
    const auto data_value = envelope.value(QString::fromLatin1(mqtt::kDataField));
    if (!mqtt::IsValidTopicLevel(message_id.toStdString()) || !mqtt::IsValidTopicLevel(source_id.toStdString()) ||
        !data_value.isObject()) {
        result.error = QStringLiteral("상품 메시지 envelope가 올바르지 않습니다.");
        return result;
    }
    if (processed_message_ids_.contains(message_id)) {
        return result;
    }

    const auto data = data_value.toObject();
    const auto work_id = StringValue(data, "workId");
    if (!mqtt::IsValidTopicLevel(work_id.toStdString())) {
        result.error = QStringLiteral("상품 메시지에 유효한 workId가 필요합니다.");
        return result;
    }
    const auto timestamp = ParseTimestamp(envelope);
    if (!timestamp.isValid()) {
        result.error = QStringLiteral("상품 메시지 timestamp가 올바르지 않습니다.");
        return result;
    }
    if (retired_work_ids_.contains(work_id) ||
        (product_.work_id == work_id && product_.updated_at.isValid() && timestamp < product_.updated_at)) {
        return result;
    }

    if (product_.work_id.isEmpty()) {
        resetForWork(work_id);
        result.switched_work = true;
    } else if (product_.work_id != work_id) {
        const bool current_is_finished = product_.processing_result == ProductProcessingResult::Success ||
                                         product_.processing_result == ProductProcessingResult::Failed;
        const bool explicit_new_work = type == mqtt::MessageType::kWorkCreated;
        if (!explicit_new_work && !current_is_finished) {
            result.error = QStringLiteral("현재 상품과 다른 workId 메시지를 무시했습니다: %1").arg(work_id);
            return result;
        }
        if (!explicit_new_work && timestamp <= product_.updated_at) {
            return result;
        }
        retired_work_ids_.insert(product_.work_id);
        resetForWork(work_id);
        result.switched_work = true;
    }

    if (type == mqtt::MessageType::kWorkCreated) {
        product_.processing_result = ProductProcessingResult::Processing;
        product_.recognition_state = ProductRecognitionState::Processing;
    }
    const auto previous_recognition_state = product_.recognition_state;

    SetIfPresent(product_.barcode, data, "barcode");
    SetIfPresent(product_.product_id, data, "productId");
    SetIfPresent(product_.product_name, data, "productName");
    SetIfPresent(product_.destination, data, "destination");
    SetIfPresent(product_.image_id, data, "imageId");
    SetIfPresent(product_.image_path, data, "imageUrl");
    if (product_.image_path.isEmpty()) {
        SetIfPresent(product_.image_path, data, "imagePath");
    }
    SetIfPresent(product_.image_checksum, data, "checksum");
    SetIfPresent(product_.image_upload_status, data, "uploadStatus");
    SetIfPresent(product_.detail, data, "message");

    const auto image = data.value(QStringLiteral("image"));
    if (image.isObject()) {
        const auto image_object = image.toObject();
        SetIfPresent(product_.image_id, image_object, "imageId");
        SetIfPresent(product_.image_path, image_object, "url");
        if (product_.image_path.isEmpty()) {
            SetIfPresent(product_.image_path, image_object, "path");
        }
        SetIfPresent(product_.image_checksum, image_object, "checksum");
        SetIfPresent(product_.image_upload_status, image_object, "uploadStatus");
    }

    const auto confidence = data.value(QStringLiteral("confidence"));
    if (confidence.isDouble() && confidence.toDouble() >= 0.0 && confidence.toDouble() <= 1.0) {
        product_.confidence = confidence.toDouble();
    }
    if (type == mqtt::MessageType::kProductInfo) {
        product_.product_info_received = true;
    }
    product_.processing_result =
        ParseProcessingResult(StringValue(data, "processingResult"), product_.processing_result);
    product_.processing_result = ParseProcessingResult(StringValue(data, "result"), product_.processing_result);
    if (type == mqtt::MessageType::kWorkCompleted && product_.processing_result != ProductProcessingResult::Success &&
        product_.processing_result != ProductProcessingResult::Failed) {
        product_.processing_result = ProductProcessingResult::Success;
    }

    const auto recognition_report = ParseRecognitionReport(data);
    if (recognition_report == RecognitionReport::Success &&
        previous_recognition_state == ProductRecognitionState::RecognitionFailed &&
        StringValue(data, "message").isEmpty()) {
        product_.detail.clear();
    }
    if (recognition_report == RecognitionReport::Failed) {
        product_.recognition_state = ProductRecognitionState::RecognitionFailed;
    } else if (recognition_report == RecognitionReport::MissingData) {
        product_.recognition_state = ProductRecognitionState::MissingData;
    } else if (recognition_report == RecognitionReport::Success) {
        product_.recognition_state = MissingRequiredFields(product_).isEmpty() ? ProductRecognitionState::Recognized
                                                                               : ProductRecognitionState::MissingData;
    } else if (product_.recognition_state != ProductRecognitionState::RecognitionFailed &&
               (product_.product_info_received || type == mqtt::MessageType::kDestinationSet)) {
        product_.recognition_state = MissingRequiredFields(product_).isEmpty() ? ProductRecognitionState::Recognized
                                                                               : ProductRecognitionState::MissingData;
    } else if (type == mqtt::MessageType::kBarcodeDetected && !product_.barcode.isEmpty()) {
        product_.recognition_state = ProductRecognitionState::Recognized;
    }

    if (product_.recognition_state == ProductRecognitionState::MissingData &&
        (product_.detail.isEmpty() || product_.detail.startsWith(QStringLiteral("누락 항목:")))) {
        product_.detail = QStringLiteral("누락 항목: %1").arg(MissingRequiredFields(product_));
    } else if (product_.detail.startsWith(QStringLiteral("누락 항목:"))) {
        product_.detail.clear();
    }

    product_.message_id = message_id;
    product_.source_id = source_id;
    product_.updated_at = timestamp;
    processed_message_ids_.insert(message_id);
    result.applied = true;
    return result;
}

const CurrentProduct& CurrentProductState::product() const noexcept { return product_; }

void CurrentProductState::resetForWork(const QString& work_id) {
    product_ = {};
    product_.work_id = work_id;
    product_.recognition_state = ProductRecognitionState::Processing;
    product_.processing_result = ProductProcessingResult::Processing;
    processed_message_ids_.clear();
}

}  // namespace logistics::control_center
