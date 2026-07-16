#include "logistics/control_center/current_product_state.hpp"

#include <QJsonObject>
#include <QString>
#include <cassert>

namespace {

QJsonObject Envelope(const QString& message_id, const QString& message_type, const QString& work_id,
                     QJsonObject data = {}, const QString& timestamp = QStringLiteral("2026-07-16T01:00:00.000Z")) {
    data.insert(QStringLiteral("workId"), work_id);
    return {
        { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
        { QStringLiteral("messageId"), message_id },
        { QStringLiteral("messageType"), message_type },
        { QStringLiteral("sourceId"), QStringLiteral("central-server") },
        { QStringLiteral("timestamp"), timestamp },
        { QStringLiteral("data"), data },
    };
}

}  // namespace

int main() {
    using logistics::control_center::CurrentProductState;
    using logistics::control_center::ProductProcessingResult;
    using logistics::control_center::ProductRecognitionState;

    CurrentProductState state;

    auto result = state.applyEnvelope(Envelope("MSG-A-1", "WORK_CREATED", "WORK-A"));
    assert(result.handled && result.applied && result.switched_work);
    assert(state.product().work_id == QStringLiteral("WORK-A"));
    assert(state.product().recognition_state == ProductRecognitionState::Processing);

    result = state.applyEnvelope(Envelope("MSG-B-EARLY", "PRODUCT_INFO", "WORK-B",
                                          { { QStringLiteral("productId"), QStringLiteral("PRODUCT-B") } }));
    assert(result.handled && !result.applied);
    assert(state.product().work_id == QStringLiteral("WORK-A"));

    const QJsonObject product_a{
        { QStringLiteral("recognitionStatus"), QStringLiteral("SUCCESS") },
        { QStringLiteral("barcode"), QStringLiteral("880000000001") },
        { QStringLiteral("productId"), QStringLiteral("PRODUCT-A") },
        { QStringLiteral("productName"), QStringLiteral("테스트 상품 A") },
        { QStringLiteral("destination"), QStringLiteral("ZONE-A") },
        { QStringLiteral("confidence"), 0.98 },
        { QStringLiteral("image"),
          QJsonObject{
              { QStringLiteral("imageId"), QStringLiteral("IMAGE-A") },
              { QStringLiteral("path"), QStringLiteral("images/WORK-A.jpg") },
              { QStringLiteral("checksum"), QStringLiteral("abc123") },
          } },
    };
    result = state.applyEnvelope(Envelope("MSG-A-2", "PRODUCT_INFO", "WORK-A", product_a));
    assert(result.applied);
    assert(state.product().recognition_state == ProductRecognitionState::Recognized);
    assert(state.product().product_id == QStringLiteral("PRODUCT-A"));
    assert(state.product().image_path == QStringLiteral("images/WORK-A.jpg"));

    result = state.applyEnvelope(Envelope("MSG-A-STALE", "PRODUCT_INFO", "WORK-A",
                                          { { QStringLiteral("productId"), QStringLiteral("STALE-PRODUCT") } },
                                          QStringLiteral("2026-07-16T00:59:00.000Z")));
    assert(result.handled && !result.applied);
    assert(state.product().product_id == QStringLiteral("PRODUCT-A"));

    result = state.applyEnvelope(Envelope("MSG-A-2", "PRODUCT_INFO", "WORK-A", product_a));
    assert(result.handled && !result.applied && result.error.isEmpty());

    result = state.applyEnvelope(
        Envelope("MSG-B-1", "WORK_CREATED", "WORK-B", {}, QStringLiteral("2026-07-16T01:01:00.000Z")));
    assert(result.applied && result.switched_work);
    assert(state.product().work_id == QStringLiteral("WORK-B"));
    assert(state.product().product_id.isEmpty());
    assert(state.product().image_path.isEmpty());

    result = state.applyEnvelope(Envelope("MSG-A-LATE", "PRODUCT_IMAGE", "WORK-A",
                                          { { QStringLiteral("imageUrl"), QStringLiteral("images/late-a.jpg") } },
                                          QStringLiteral("2026-07-16T01:01:01.000Z")));
    assert(result.handled && !result.applied);
    assert(state.product().work_id == QStringLiteral("WORK-B"));
    assert(state.product().image_path.isEmpty());

    const QJsonObject recognition_failed{
        { QStringLiteral("recognitionStatus"), QStringLiteral("FAILED") },
        { QStringLiteral("message"), QStringLiteral("바코드를 찾지 못했습니다.") },
    };
    result = state.applyEnvelope(Envelope("MSG-B-2", "BARCODE_DETECTED", "WORK-B", recognition_failed,
                                          QStringLiteral("2026-07-16T01:01:01.000Z")));
    assert(result.applied);
    assert(state.product().recognition_state == ProductRecognitionState::RecognitionFailed);

    const QJsonObject missing_destination{
        { QStringLiteral("recognitionStatus"), QStringLiteral("SUCCESS") },
        { QStringLiteral("barcode"), QStringLiteral("880000000002") },
        { QStringLiteral("productId"), QStringLiteral("PRODUCT-B") },
        { QStringLiteral("productName"), QStringLiteral("테스트 상품 B") },
    };
    result = state.applyEnvelope(
        Envelope("MSG-B-3", "PRODUCT_INFO", "WORK-B", missing_destination, QStringLiteral("2026-07-16T01:01:02.000Z")));
    assert(result.applied);
    assert(state.product().recognition_state == ProductRecognitionState::MissingData);

    result = state.applyEnvelope(Envelope("MSG-B-4", "DESTINATION_SET", "WORK-B",
                                          { { QStringLiteral("destination"), QStringLiteral("ZONE-B") } },
                                          QStringLiteral("2026-07-16T01:01:03.000Z")));
    assert(result.applied);
    assert(state.product().recognition_state == ProductRecognitionState::Recognized);

    result = state.applyEnvelope(Envelope("MSG-B-5", "WORK_COMPLETED", "WORK-B",
                                          { { QStringLiteral("result"), QStringLiteral("SUCCESS") } },
                                          QStringLiteral("2026-07-16T01:01:04.000Z")));
    assert(result.applied);
    assert(state.product().processing_result == ProductProcessingResult::Success);

    result = state.applyEnvelope(
        Envelope("MSG-A-VERY-LATE", "PRODUCT_INFO", "WORK-A", product_a, QStringLiteral("2026-07-16T01:10:00.000Z")));
    assert(result.handled && !result.applied);
    assert(state.product().work_id == QStringLiteral("WORK-B"));

    return 0;
}
