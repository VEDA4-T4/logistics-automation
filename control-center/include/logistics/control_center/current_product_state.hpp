#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QString>

namespace logistics::control_center {

enum class ProductRecognitionState { Waiting, Processing, Recognized, RecognitionFailed, MissingData };
enum class ProductProcessingResult { Pending, Processing, Success, Failed };

struct CurrentProduct {
    QString work_id;
    QString message_id;
    QString source_id;
    QString barcode;
    QString product_id;
    QString product_name;
    QString destination;
    QString image_id;
    QString image_path;
    QString image_checksum;
    QString image_upload_status;
    QString detail;
    QDateTime updated_at;
    double confidence{ -1.0 };
    ProductRecognitionState recognition_state{ ProductRecognitionState::Waiting };
    ProductProcessingResult processing_result{ ProductProcessingResult::Pending };
    bool product_info_received{ false };
};

struct ProductUpdateResult {
    bool handled{ false };
    bool applied{ false };
    bool switched_work{ false };
    QString error;
};

class CurrentProductState final {
public:
    [[nodiscard]] ProductUpdateResult applyEnvelope(const QJsonObject& envelope);
    [[nodiscard]] const CurrentProduct& product() const noexcept;

private:
    void resetForWork(const QString& work_id);

    CurrentProduct product_;
    QSet<QString> processed_message_ids_;
    QSet<QString> retired_work_ids_;
};

}  // namespace logistics::control_center
