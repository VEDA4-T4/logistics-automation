#include "logistics/control_center/product_result_panel.hpp"

#include <QDebug>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
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

QString StatusPillStyle(const char* background, const char* foreground, const char* border) {
    return QStringLiteral(
               "background-color:%1;color:%2;border:1px solid %3;border-radius:8px;"
               "font-size:12px;font-weight:700;padding:6px 10px;")
        .arg(QString::fromLatin1(background), QString::fromLatin1(foreground), QString::fromLatin1(border));
}

QLabel* AddValueRow(QGridLayout* layout, int row, const QString& title, QWidget* parent) {
    auto* title_label = new QLabel(title, parent);
    title_label->setMinimumWidth(58);
    title_label->setStyleSheet("color:#94a3b8;font-size:11px;font-weight:700;");
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
    setMinimumWidth(310);
    setMaximumWidth(360);
    setMinimumHeight(420);
    setStyleSheet(
        "#productResultPanel{background-color:#0b1220;border:1px solid #243247;border-radius:12px;}"
        "#productInfoCard,#productDetailCard{background-color:#111827;border:1px solid #2b3a50;"
        "border-radius:10px;}"
        "QLabel{color:#e2e8f0;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto* header_layout = new QHBoxLayout();
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(8);
    auto* header_text_layout = new QVBoxLayout();
    header_text_layout->setContentsMargins(0, 0, 0, 0);
    header_text_layout->setSpacing(2);
    auto* eyebrow = new QLabel(QStringLiteral("VISION RESULT"), this);
    eyebrow->setStyleSheet("color:#38bdf8;font-size:10px;font-weight:800;letter-spacing:1px;");
    auto* title = new QLabel(QStringLiteral("현재 상품"), this);
    title->setStyleSheet("color:#f8fafc;font-size:20px;font-weight:800;");
    auto* subtitle = new QLabel(QStringLiteral("실시간 인식 및 공정 결과"), this);
    subtitle->setStyleSheet("color:#94a3b8;font-size:11px;");
    header_text_layout->addWidget(eyebrow);
    header_text_layout->addWidget(title);
    header_text_layout->addWidget(subtitle);
    header_layout->addLayout(header_text_layout);
    header_layout->addStretch();

    auto* status_layout = new QHBoxLayout();
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(8);
    recognition_status_ = new QLabel(QStringLiteral("상품 데이터 대기 중"), this);
    recognition_status_->setAlignment(Qt::AlignCenter);
    recognition_status_->setMinimumHeight(32);
    recognition_status_->setStyleSheet(StatusPillStyle("#172033", "#cbd5e1", "#334155"));
    processing_status_ = new QLabel(QStringLiteral("처리 대기"), this);
    processing_status_->setAlignment(Qt::AlignCenter);
    processing_status_->setMinimumHeight(32);
    processing_status_->setStyleSheet(StatusPillStyle("#172033", "#cbd5e1", "#334155"));
    status_layout->addWidget(recognition_status_, 1);
    status_layout->addWidget(processing_status_, 1);

    image_label_ = new QLabel(this);
    image_label_->setMinimumHeight(140);
    image_label_->setMaximumHeight(150);
    image_label_->setAlignment(Qt::AlignCenter);
    setImagePlaceholder(QStringLiteral("상품 이미지 대기 중"));

    auto* info_card = new QFrame(this);
    info_card->setObjectName(QStringLiteral("productInfoCard"));
    auto* info_layout = new QVBoxLayout(info_card);
    info_layout->setContentsMargins(12, 11, 12, 11);
    info_layout->setSpacing(8);
    auto* info_title = new QLabel(QStringLiteral("상품 정보"), info_card);
    info_title->setStyleSheet("color:#cbd5e1;font-size:11px;font-weight:800;");
    auto* fields = new QGridLayout();
    fields->setHorizontalSpacing(10);
    fields->setVerticalSpacing(6);
    fields->setColumnStretch(1, 1);
    work_id_value_ = AddValueRow(fields, 0, QStringLiteral("작업"), info_card);
    barcode_value_ = AddValueRow(fields, 1, QStringLiteral("바코드"), info_card);
    product_id_value_ = AddValueRow(fields, 2, QStringLiteral("상품 ID"), info_card);
    product_name_value_ = AddValueRow(fields, 3, QStringLiteral("상품명"), info_card);
    destination_value_ = AddValueRow(fields, 4, QStringLiteral("목적지"), info_card);
    confidence_value_ = AddValueRow(fields, 5, QStringLiteral("신뢰도"), info_card);
    updated_at_value_ = AddValueRow(fields, 6, QStringLiteral("갱신"), info_card);
    info_layout->addWidget(info_title);
    info_layout->addLayout(fields);

    auto* detail_card = new QFrame(this);
    detail_card->setObjectName(QStringLiteral("productDetailCard"));
    auto* detail_layout = new QVBoxLayout(detail_card);
    detail_layout->setContentsMargins(11, 9, 11, 9);
    detail_layout->setSpacing(3);
    auto* detail_title = new QLabel(QStringLiteral("처리 메시지"), detail_card);
    detail_title->setStyleSheet("color:#94a3b8;font-size:10px;font-weight:800;");
    detail_value_ = new QLabel(QStringLiteral("MQTT 상품 메시지를 기다리고 있습니다."), detail_card);
    detail_value_->setWordWrap(true);
    detail_value_->setStyleSheet("color:#cbd5e1;font-size:11px;");
    detail_layout->addWidget(detail_title);
    detail_layout->addWidget(detail_value_);

    layout->addLayout(header_layout);
    layout->addLayout(status_layout);
    layout->addWidget(image_label_);
    layout->addWidget(info_card);
    layout->addWidget(detail_card);
    layout->addStretch();
}

void ProductResultPanel::setCurrentProduct(const CurrentProduct& product) {
    if (current_work_id_ != product.work_id) {
        current_work_id_ = product.work_id;
        current_image_path_.clear();
        if (active_image_reply_ != nullptr)
            active_image_reply_->abort();
    }

    switch (product.recognition_state) {
        case ProductRecognitionState::Waiting:
            recognition_status_->setText(QStringLiteral("상품 데이터 대기 중"));
            recognition_status_->setStyleSheet(StatusPillStyle("#172033", "#cbd5e1", "#334155"));
            break;
        case ProductRecognitionState::Processing:
            recognition_status_->setText(QStringLiteral("인식 중"));
            recognition_status_->setStyleSheet(StatusPillStyle("#3b2a0d", "#fbbf24", "#92400e"));
            break;
        case ProductRecognitionState::Recognized:
            recognition_status_->setText(QStringLiteral("인식 성공"));
            recognition_status_->setStyleSheet(StatusPillStyle("#062c20", "#86efac", "#166534"));
            break;
        case ProductRecognitionState::RecognitionFailed:
            recognition_status_->setText(QStringLiteral("인식 실패"));
            recognition_status_->setStyleSheet(StatusPillStyle("#3b1014", "#fca5a5", "#991b1b"));
            break;
        case ProductRecognitionState::MissingData:
            recognition_status_->setText(QStringLiteral("데이터 누락"));
            recognition_status_->setStyleSheet(StatusPillStyle("#3b210b", "#fdba74", "#9a3412"));
            break;
    }

    processing_status_->setText(ProcessingText(product.processing_result));
    switch (product.processing_result) {
        case ProductProcessingResult::Pending:
            processing_status_->setStyleSheet(StatusPillStyle("#172033", "#cbd5e1", "#334155"));
            break;
        case ProductProcessingResult::Processing:
            processing_status_->setStyleSheet(StatusPillStyle("#102a43", "#7dd3fc", "#0369a1"));
            break;
        case ProductProcessingResult::Success:
            processing_status_->setStyleSheet(StatusPillStyle("#062c20", "#86efac", "#166534"));
            break;
        case ProductProcessingResult::Failed:
            processing_status_->setStyleSheet(StatusPillStyle("#3b1014", "#fca5a5", "#991b1b"));
            break;
    }
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

    if (!product.image_path.isEmpty() && current_image_path_ != product.image_path) {
        current_image_path_ = product.image_path;
        loadImage(product);
    }
}

void ProductResultPanel::setValue(QLabel* label, const QString& value) {
    label->setText(value.isEmpty() ? QStringLiteral("데이터 없음") : value);
    label->setStyleSheet(value.isEmpty() ? "color:#fbbf24;font-size:12px;font-weight:700;"
                                         : "color:#f1f5f9;font-size:12px;font-weight:600;");
}

void ProductResultPanel::setImagePlaceholder(const QString& text, bool is_error) {
    image_label_->clear();
    image_label_->setText(text);
    image_label_->setStyleSheet(is_error
                                    ? "background:#2b1014;color:#fca5a5;border:1px solid #7f1d1d;border-radius:10px;"
                                    : "background:#070d18;color:#64748b;border:1px solid #263449;border-radius:10px;");
}

void ProductResultPanel::loadImage(const CurrentProduct& product) {
    if (active_image_reply_ != nullptr)
        active_image_reply_->abort();
    if (product.image_path.isEmpty())
        return;
    QUrl image_url(product.image_path);
    if (image_url.isRelative())
        image_url = image_base_url_.resolved(image_url);
    if (!image_url.isValid() || (image_url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0 &&
                                 image_url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)) {
        const QString detail = QStringLiteral("이미지 경로 오류: %1").arg(product.image_path);
        qWarning().noquote() << "[control-center][http] image download failed:" << detail;
        image_label_->setToolTip(detail);
        return;
    }

    QNetworkRequest request(image_url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = network_manager_->get(request);
    active_image_reply_ = reply;
    const auto requested_work_id = product.work_id;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (received > kMaximumProductImageBytes || total > kMaximumProductImageBytes)
            reply->abort();
    });
    const QPointer<QNetworkReply> guarded_reply(reply);
    QTimer::singleShot(5000, this, [guarded_reply]() {
        if (guarded_reply != nullptr && guarded_reply->isRunning())
            guarded_reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requested_work_id]() {
        const bool active = active_image_reply_ == reply;
        if (active)
            active_image_reply_ = nullptr;
        reply->deleteLater();
        if (!active || requested_work_id != current_work_id_)
            return;
        if (reply->error() != QNetworkReply::NoError) {
            const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString detail = QStringLiteral("URL: %1\nHTTP: %2\nError: %3")
                                       .arg(reply->url().toString())
                                       .arg(http_status == 0 ? QStringLiteral("N/A") : QString::number(http_status))
                                       .arg(reply->errorString());
            qWarning().noquote() << "[control-center][http] image download failed:" << detail;
            image_label_->setToolTip(detail);
            return;
        }
        const auto payload = reply->readAll();
        QPixmap image;
        if (payload.size() > kMaximumProductImageBytes || !image.loadFromData(payload)) {
            const QString detail = QStringLiteral("이미지 형식 오류: %1").arg(reply->url().toString());
            qWarning().noquote() << "[control-center][http] image decode failed:" << detail;
            image_label_->setToolTip(detail);
            return;
        }
        image_label_->setText({});
        image_label_->setStyleSheet("background:#070d18;border:1px solid #263449;border-radius:10px;");
        image_label_->setPixmap(image.scaled(292, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        image_label_->setToolTip({});
    });
}

}  // namespace logistics::control_center
