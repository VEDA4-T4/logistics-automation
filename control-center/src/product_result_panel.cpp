#include "logistics/control_center/product_result_panel.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace logistics::control_center {
namespace {

constexpr qint64 kMaximumProductImageBytes = 10 * 1024 * 1024;

QLabel* AddValueRow(QGridLayout* layout, int row, const QString& title, QWidget* parent) {
    auto* title_label = new QLabel(title, parent);
    title_label->setStyleSheet("color: #9ca3af; font-size: 12px;");
    auto* value_label = new QLabel(QStringLiteral("데이터 없음"), parent);
    value_label->setWordWrap(true);
    layout->addWidget(title_label, row, 0, Qt::AlignTop);
    layout->addWidget(value_label, row, 1);
    return value_label;
}

QString ProcessingText(ProductProcessingResult result) {
    switch (result) {
        case ProductProcessingResult::Pending:
            return QStringLiteral("처리 대기");
        case ProductProcessingResult::Processing:
            return QStringLiteral("처리 중");
        case ProductProcessingResult::Success:
            return QStringLiteral("처리 완료");
        case ProductProcessingResult::Failed:
            return QStringLiteral("처리 실패");
    }
    return QStringLiteral("처리 상태 없음");
}

}  // namespace

ProductResultPanel::ProductResultPanel(QUrl image_base_url, QWidget* parent)
    : QWidget(parent), image_base_url_(std::move(image_base_url)), network_manager_(new QNetworkAccessManager(this)) {
    setObjectName(QStringLiteral("productResultPanel"));
    setMinimumWidth(300);
    setMaximumWidth(320);
    setMinimumHeight(300);
    setStyleSheet(
        "#productResultPanel { background-color: #111827; border-left: 1px solid #374151;"
        "border-bottom: 1px solid #374151; } QLabel { color: #e5e7eb; }");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("현재 상품 및 인식 결과"), this);
    title->setStyleSheet("font-size: 18px; font-weight: 800;");

    auto* status_layout = new QGridLayout();
    recognition_status_ = new QLabel(QStringLiteral("상품 데이터 대기 중"), this);
    recognition_status_->setAlignment(Qt::AlignCenter);
    recognition_status_->setStyleSheet(
        "background-color: #1f2937; color: #d1d5db; border-radius: 5px; font-weight: 700; padding: 5px;");
    processing_status_ = new QLabel(QStringLiteral("처리 대기"), this);
    processing_status_->setAlignment(Qt::AlignCenter);
    processing_status_->setStyleSheet(
        "background-color: #1f2937; color: #d1d5db; border-radius: 5px; font-weight: 700; padding: 5px;");
    status_layout->addWidget(recognition_status_, 0, 0);
    status_layout->addWidget(processing_status_, 0, 1);

    image_label_ = new QLabel(this);
    image_label_->setObjectName(QStringLiteral("currentProductImage"));
    image_label_->setMinimumHeight(105);
    image_label_->setMaximumHeight(125);
    image_label_->setAlignment(Qt::AlignCenter);
    image_label_->setScaledContents(false);
    setImagePlaceholder(QStringLiteral("상품 이미지 대기 중"));

    auto* fields = new QGridLayout();
    fields->setHorizontalSpacing(8);
    fields->setVerticalSpacing(2);
    fields->setColumnStretch(1, 1);
    work_id_value_ = AddValueRow(fields, 0, QStringLiteral("작업"), this);
    barcode_value_ = AddValueRow(fields, 1, QStringLiteral("바코드"), this);
    product_id_value_ = AddValueRow(fields, 2, QStringLiteral("상품 ID"), this);
    product_name_value_ = AddValueRow(fields, 3, QStringLiteral("상품명"), this);
    destination_value_ = AddValueRow(fields, 4, QStringLiteral("목적지"), this);
    confidence_value_ = AddValueRow(fields, 5, QStringLiteral("신뢰도"), this);
    updated_at_value_ = AddValueRow(fields, 6, QStringLiteral("갱신"), this);

    detail_value_ = new QLabel(QStringLiteral("MQTT 상품 메시지를 기다리고 있습니다."), this);
    detail_value_->setObjectName(QStringLiteral("productResultDetail"));
    detail_value_->setWordWrap(true);
    detail_value_->setStyleSheet(
        "background-color: #1f2937; color: #9ca3af; border-radius: 4px; padding: 5px; font-size: 12px;");

    layout->addWidget(title);
    layout->addLayout(status_layout);
    layout->addWidget(image_label_);
    layout->addLayout(fields);
    layout->addWidget(detail_value_);
}

void ProductResultPanel::setCurrentProduct(const CurrentProduct& product) {
    const bool work_changed = current_work_id_ != product.work_id;
    if (work_changed) {
        current_work_id_ = product.work_id;
        current_image_path_.clear();
        if (active_image_reply_ != nullptr) {
            active_image_reply_->abort();
        }
        setImagePlaceholder(QStringLiteral("상품 이미지 대기 중"));
    }

    switch (product.recognition_state) {
        case ProductRecognitionState::Waiting:
            recognition_status_->setText(QStringLiteral("상품 데이터 대기 중"));
            recognition_status_->setStyleSheet(
                "background-color: #1f2937; color: #d1d5db; border-radius: 5px; font-weight: 700; padding: 5px;");
            break;
        case ProductRecognitionState::Processing:
            recognition_status_->setText(QStringLiteral("인식 중"));
            recognition_status_->setStyleSheet(
                "background-color: #422006; color: #fbbf24; border-radius: 5px; font-weight: 700; padding: 5px;");
            break;
        case ProductRecognitionState::Recognized:
            recognition_status_->setText(QStringLiteral("인식 성공"));
            recognition_status_->setStyleSheet(
                "background-color: #052e16; color: #86efac; border-radius: 5px; font-weight: 700; padding: 5px;");
            break;
        case ProductRecognitionState::RecognitionFailed:
            recognition_status_->setText(QStringLiteral("인식 실패"));
            recognition_status_->setStyleSheet(
                "background-color: #450a0a; color: #fca5a5; border-radius: 5px; font-weight: 700; padding: 5px;");
            break;
        case ProductRecognitionState::MissingData:
            recognition_status_->setText(QStringLiteral("데이터 누락"));
            recognition_status_->setStyleSheet(
                "background-color: #451a03; color: #fdba74; border-radius: 5px; font-weight: 700; padding: 5px;");
            break;
    }

    processing_status_->setText(ProcessingText(product.processing_result));
    const bool processing_failed = product.processing_result == ProductProcessingResult::Failed;
    const bool processing_succeeded = product.processing_result == ProductProcessingResult::Success;
    processing_status_->setStyleSheet(
        processing_failed
            ? "background-color: #450a0a; color: #fca5a5; border-radius: 5px; font-weight: 700; padding: 5px;"
        : processing_succeeded
            ? "background-color: #052e16; color: #86efac; border-radius: 5px; font-weight: 700; padding: 5px;"
            : "background-color: #1e3a5f; color: #93c5fd; border-radius: 5px; font-weight: 700; padding: 5px;");

    setValue(work_id_value_, product.work_id);
    setValue(barcode_value_, product.barcode);
    setValue(product_id_value_, product.product_id);
    setValue(product_name_value_, product.product_name);
    setValue(destination_value_, product.destination);
    setValue(confidence_value_,
             product.confidence >= 0.0 ? QStringLiteral("%1%").arg(product.confidence * 100.0, 0, 'f', 1) : QString{});
    setValue(updated_at_value_, product.updated_at.isValid()
                                    ? product.updated_at.toLocalTime().toString(QStringLiteral("MM-dd HH:mm:ss"))
                                    : QString{});

    detail_value_->setText(product.detail.isEmpty() ? QStringLiteral("추가 처리 정보가 없습니다.") : product.detail);
    detail_value_->setToolTip(QStringLiteral("messageId: %1\nimageId: %2\nchecksum: %3")
                                  .arg(product.message_id, product.image_id, product.image_checksum));

    if (current_image_path_ != product.image_path) {
        current_image_path_ = product.image_path;
        loadImage(product);
    } else if (product.image_path.isEmpty() && product.recognition_state != ProductRecognitionState::Processing &&
               product.recognition_state != ProductRecognitionState::Waiting) {
        setImagePlaceholder(QStringLiteral("상품 이미지 없음"));
    }
}

void ProductResultPanel::setValue(QLabel* label, const QString& value) {
    if (value.isEmpty()) {
        label->setText(QStringLiteral("데이터 없음"));
        label->setStyleSheet("color: #fbbf24; font-size: 12px; font-weight: 700;");
        return;
    }
    label->setText(value);
    label->setStyleSheet("color: #f3f4f6; font-size: 12px;");
}

void ProductResultPanel::setImagePlaceholder(const QString& text, bool is_error) {
    image_label_->clear();
    image_label_->setText(text);
    image_label_->setStyleSheet(
        is_error ? "background-color: #450a0a; color: #fca5a5; border: 1px solid #7f1d1d; border-radius: 5px;"
                 : "background-color: #030712; color: #6b7280; border: 1px solid #374151; border-radius: 5px;");
}

void ProductResultPanel::loadImage(const CurrentProduct& product) {
    if (active_image_reply_ != nullptr) {
        active_image_reply_->abort();
    }
    if (product.image_path.isEmpty()) {
        setImagePlaceholder(QStringLiteral("상품 이미지 없음"));
        return;
    }

    QUrl image_url(product.image_path);
    if (image_url.isRelative()) {
        image_url = image_base_url_.resolved(image_url);
    }
    if (!image_url.isValid() || (image_url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0 &&
                                 image_url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)) {
        setImagePlaceholder(QStringLiteral("이미지 경로 오류"), true);
        return;
    }

    setImagePlaceholder(QStringLiteral("상품 이미지 불러오는 중…"));
    QNetworkRequest request(image_url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = network_manager_->get(request);
    active_image_reply_ = reply;
    const auto requested_work_id = product.work_id;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (received > kMaximumProductImageBytes || total > kMaximumProductImageBytes) {
            reply->abort();
        }
    });
    const QPointer<QNetworkReply> guarded_reply(reply);
    QTimer::singleShot(5000, this, [guarded_reply]() {
        if (guarded_reply != nullptr && guarded_reply->isRunning()) {
            guarded_reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requested_work_id]() {
        const bool is_active_request = active_image_reply_ == reply;
        if (is_active_request) {
            active_image_reply_ = nullptr;
        }
        reply->deleteLater();
        if (!is_active_request || requested_work_id != current_work_id_) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            setImagePlaceholder(QStringLiteral("상품 이미지 불러오기 실패"), true);
            image_label_->setToolTip(reply->errorString());
            return;
        }

        const auto payload = reply->readAll();
        QPixmap image;
        if (payload.size() > kMaximumProductImageBytes || !image.loadFromData(payload)) {
            setImagePlaceholder(QStringLiteral("상품 이미지 형식 오류"), true);
            return;
        }
        image_label_->setText({});
        image_label_->setStyleSheet("background-color: #030712; border: 1px solid #374151; border-radius: 5px;");
        image_label_->setPixmap(image.scaled(292, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        image_label_->setToolTip({});
    });
}

}  // namespace logistics::control_center
