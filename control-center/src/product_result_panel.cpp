#include "logistics/control_center/product_result_panel.hpp"

#include <QDebug>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace logistics::control_center {
namespace {

constexpr qint64 kMaximumProductImageBytes = 10 * 1024 * 1024;
constexpr auto kSurfaceColor = "#141d26";
constexpr auto kBorderColor = "#24313d";
constexpr auto kPrimaryTextColor = "#e7eef3";

QString StatusPillStyle(const char* background, const char* foreground, const char* border) {
    return QStringLiteral(
               "background-color:%1;color:%2;border:1px solid %3;border-radius:4px;"
               "font-size:11px;font-weight:700;padding:4px 8px;")
        .arg(QString::fromLatin1(background), QString::fromLatin1(foreground), QString::fromLatin1(border));
}

QLabel* AddValueRow(QGridLayout* layout, int row, int column, const QString& title, QWidget* parent,
                    int value_column_span = 1) {
    auto* title_label = new QLabel(title, parent);
    title_label->setMinimumWidth(36);
    title_label->setStyleSheet("color:#91a3b0;font-size:10px;font-weight:600;");
    auto* value_label = new QLabel(QStringLiteral("데이터 없음"), parent);
    value_label->setStyleSheet("color:#cca700;font-size:10px;font-weight:700;");
    value_label->setWordWrap(false);
    value_label->setMinimumWidth(0);
    value_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(title_label, row, column);
    layout->addWidget(value_label, row, column + 1, 1, value_column_span);
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

QString PositionText(const LineTracerPositionStatus& position) {
    const auto area = position.area.compare(QStringLiteral("DEPARTURE"), Qt::CaseInsensitive) == 0
                          ? QStringLiteral("출발")
                          : QStringLiteral("도착");
    return QStringLiteral("%1 %2").arg(area, position.location);
}

QString DestinationText(const QString& destination) {
    const auto normalized = destination.trimmed().toUpper();
    if (normalized.size() == 1 && normalized.front() >= QLatin1Char('A') && normalized.front() <= QLatin1Char('C')) {
        return normalized;
    }
    const auto route = DestinationRouteIndex(normalized);
    return route.has_value() ? QString(QChar(QLatin1Char('A').unicode() + *route - 1)) : normalized;
}

QString TrackingText(const QString& work_id, const QString& destination, const QList<ProcessUnitStatus>& processes) {
    const ProcessUnitStatus* current = nullptr;
    for (const auto& process : processes) {
        if (process.work_id == work_id && !process.work_completed) {
            current = &process;
        }
    }
    if (current == nullptr) {
        const auto target = DestinationText(destination);
        return QStringLiteral("위치 · 확인 중  |  경로 · %1")
            .arg(target.isEmpty() ? QStringLiteral("확인 중") : QStringLiteral("도착 %1").arg(target));
    }

    QString location = QStringLiteral("%1 · %2").arg(current->display_name, current->current_state);
    if (current->confirmed_position.has_value()) {
        location = PositionText(*current->confirmed_position);
    }

    QString route;
    if (current->departure_position.has_value() || current->target_position.has_value()) {
        const auto departure = current->departure_position.has_value() ? PositionText(*current->departure_position)
                                                                       : QStringLiteral("출발 확인 중");
        const auto target = current->target_position.has_value() ? PositionText(*current->target_position)
                                                                 : QStringLiteral("도착 확인 중");
        route = QStringLiteral("%1 → %2").arg(departure, target);
    } else {
        const auto target = DestinationText(current->destination.isEmpty() ? destination : current->destination);
        route = target.isEmpty() ? QStringLiteral("확인 중") : QStringLiteral("도착 %1").arg(target);
    }
    return QStringLiteral("위치 · %1  |  경로 · %2").arg(location, route);
}

}  // namespace

ProductResultPanel::ProductResultPanel(QUrl image_base_url, QWidget* parent)
    : QWidget(parent), image_base_url_(std::move(image_base_url)), network_manager_(new QNetworkAccessManager(this)) {
    setObjectName(QStringLiteral("productResultPanel"));
    setMinimumWidth(0);
    setMinimumHeight(180);
    setStyleSheet(
        "#productResultPanel{background-color:#141d26;border:0;}"
        "#productInfoCard,#productDetailCard{background-color:#141d26;border:1px solid #24313d;"
        "border-radius:6px;}");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);

    auto* header_layout = new QHBoxLayout();
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(8);
    auto* header_text_layout = new QVBoxLayout();
    header_text_layout->setContentsMargins(0, 0, 0, 0);
    header_text_layout->setSpacing(2);
    auto* eyebrow = new QLabel(QStringLiteral("VISION RESULT"), this);
    eyebrow->setStyleSheet("color:#4daafc;font-size:9px;font-weight:700;letter-spacing:1px;");
    auto* title = new QLabel(QStringLiteral("현재 상품"), this);
    title->setStyleSheet("color:#e7eef3;font-size:17px;font-weight:700;");
    tracking_status_ = new QLabel(QStringLiteral("위치 · 확인 중  |  경로 · 확인 중"), this);
    tracking_status_->setObjectName(QStringLiteral("workTrackingStatus"));
    tracking_status_->setStyleSheet("color:#91a3b0;font-size:9px;font-weight:600;");
    tracking_status_->setMinimumWidth(0);
    tracking_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header_text_layout->addWidget(eyebrow);
    header_text_layout->addWidget(title);
    header_text_layout->addWidget(tracking_status_);
    header_layout->addLayout(header_text_layout);
    header_layout->addStretch();

    auto* status_row = new QWidget(this);
    status_row->setObjectName(QStringLiteral("productStatusRow"));
    auto* status_layout = new QHBoxLayout(status_row);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(4);

    recognition_status_ = new QLabel(QStringLiteral("상품 데이터 대기 중"), status_row);
    recognition_status_->setAlignment(Qt::AlignCenter);
    recognition_status_->setMinimumHeight(24);
    recognition_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    recognition_status_->setStyleSheet(StatusPillStyle(kSurfaceColor, kPrimaryTextColor, kBorderColor));
    processing_status_ = new QLabel(QStringLiteral("처리 대기"), status_row);
    processing_status_->setAlignment(Qt::AlignCenter);
    processing_status_->setMinimumHeight(24);
    processing_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    processing_status_->setStyleSheet(StatusPillStyle(kSurfaceColor, kPrimaryTextColor, kBorderColor));
    status_layout->addWidget(recognition_status_);
    status_layout->addWidget(processing_status_);
    header_layout->addWidget(status_row);

    image_label_ = new QLabel(this);
    image_label_->setObjectName(QStringLiteral("productImage"));
    image_label_->setMinimumSize(0, 0);
    image_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    image_label_->setAlignment(Qt::AlignCenter);
    setImagePlaceholder(QStringLiteral("상품 이미지 대기 중"));

    auto* info_card = new QFrame(this);
    info_card->setObjectName(QStringLiteral("productInfoCard"));
    auto* info_layout = new QVBoxLayout(info_card);
    info_layout->setContentsMargins(9, 7, 9, 7);
    info_layout->setSpacing(5);
    auto* info_title = new QLabel(QStringLiteral("상품 정보"), info_card);
    info_title->setStyleSheet("color:#e7eef3;font-size:10px;font-weight:700;");
    auto* fields = new QGridLayout();
    fields->setHorizontalSpacing(6);
    fields->setVerticalSpacing(3);
    fields->setColumnStretch(1, 1);
    fields->setColumnStretch(3, 1);
    work_id_value_ = AddValueRow(fields, 0, 0, QStringLiteral("작업"), info_card, 3);
    barcode_value_ = AddValueRow(fields, 1, 0, QStringLiteral("바코드"), info_card);
    product_id_value_ = AddValueRow(fields, 1, 2, QStringLiteral("상품 ID"), info_card);
    product_name_value_ = AddValueRow(fields, 2, 0, QStringLiteral("상품명"), info_card);
    destination_value_ = AddValueRow(fields, 2, 2, QStringLiteral("목적지"), info_card);
    confidence_value_ = AddValueRow(fields, 3, 0, QStringLiteral("신뢰도"), info_card);
    updated_at_value_ = AddValueRow(fields, 3, 2, QStringLiteral("갱신"), info_card);
    info_layout->addWidget(info_title);
    info_layout->addLayout(fields);

    auto* detail_card = new QFrame(this);
    detail_card->setObjectName(QStringLiteral("productDetailCard"));
    auto* detail_layout = new QVBoxLayout(detail_card);
    detail_layout->setContentsMargins(9, 6, 9, 6);
    detail_layout->setSpacing(2);
    auto* detail_title = new QLabel(QStringLiteral("처리 메시지"), detail_card);
    detail_title->setStyleSheet("color:#91a3b0;font-size:9px;font-weight:700;");
    detail_value_ = new QLabel(QStringLiteral("MQTT 상품 메시지를 기다리고 있습니다."), detail_card);
    detail_value_->setWordWrap(true);
    detail_value_->setMaximumHeight(30);
    detail_value_->setStyleSheet("color:#e7eef3;font-size:10px;");
    detail_layout->addWidget(detail_title);
    detail_layout->addWidget(detail_value_);

    auto* metadata = new QWidget(this);
    metadata->setObjectName(QStringLiteral("productMetadata"));
    metadata->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* metadata_layout = new QVBoxLayout(metadata);
    metadata_layout->setContentsMargins(0, 0, 0, 0);
    metadata_layout->setSpacing(6);
    metadata_layout->addWidget(info_card, 1);
    metadata_layout->addWidget(detail_card);
    auto* content_layout = new QHBoxLayout();
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(8);
    auto* work_list_panel = new QWidget(this);
    auto* work_list_layout = new QVBoxLayout(work_list_panel);
    work_list_layout->setContentsMargins(0, 0, 0, 0);
    work_list_layout->setSpacing(4);
    auto* work_list_title = new QLabel(QStringLiteral("작업 중"), work_list_panel);
    work_list_title->setStyleSheet("color:#91a3b0;font-size:9px;font-weight:700;");
    active_work_list_ = new QListWidget(work_list_panel);
    active_work_list_->setObjectName(QStringLiteral("activeWorkList"));
    active_work_list_->setMinimumWidth(110);
    active_work_list_->setMaximumWidth(170);
    active_work_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    active_work_list_->setToolTip(QStringLiteral("현재 작업 중인 항목"));
    active_work_list_->setStyleSheet(
        "QListWidget{background:#141d26;color:#e7eef3;border:1px solid #24313d;border-radius:6px;outline:0;}"
        "QListWidget::item{padding:7px 6px;border-bottom:1px solid #24313d;}"
        "QListWidget::item:selected{background:#264f78;color:#ffffff;}");
    work_list_layout->addWidget(work_list_title);
    work_list_layout->addWidget(active_work_list_, 1);
    content_layout->addWidget(work_list_panel);
    content_layout->addWidget(image_label_, 3);
    content_layout->addWidget(metadata, 2);

    layout->addLayout(header_layout);
    layout->addLayout(content_layout, 1);

    connect(active_work_list_, &QListWidget::currentRowChanged, this, [this]() { showSelectedWork(); });
}

void ProductResultPanel::setActiveWorks(const QList<CurrentProduct>& products,
                                        const QList<ProcessUnitStatus>& processes) {
    const auto selected_work_id = active_work_list_->currentItem() == nullptr
                                      ? QString{}
                                      : active_work_list_->currentItem()->data(Qt::UserRole).toString();
    active_products_.clear();
    processes_ = processes;
    for (const auto& product : products) {
        const bool active = std::any_of(processes.cbegin(), processes.cend(), [&product](const auto& process) {
            return process.work_id == product.work_id && !process.work_completed;
        });
        if (active || (product.processing_result != ProductProcessingResult::Success &&
                       product.processing_result != ProductProcessingResult::Failed)) {
            active_products_.append(product);
        }
    }
    for (const auto& process : processes) {
        if (process.work_id.isEmpty() || process.work_completed ||
            std::any_of(active_products_.cbegin(), active_products_.cend(),
                        [&process](const auto& product) { return product.work_id == process.work_id; })) {
            continue;
        }
        CurrentProduct product;
        product.work_id = process.work_id;
        product.destination = process.destination;
        product.processing_result = ProductProcessingResult::Processing;
        active_products_.append(product);
    }
    const QSignalBlocker blocker(active_work_list_);
    active_work_list_->clear();
    for (const auto& product : active_products_) {
        auto* item = new QListWidgetItem(product.work_id, active_work_list_);
        item->setData(Qt::UserRole, product.work_id);
        item->setToolTip(product.work_id);
    }
    auto selected_index = -1;
    for (int index = 0; index < active_work_list_->count(); ++index) {
        if (active_work_list_->item(index)->data(Qt::UserRole).toString() == selected_work_id) {
            selected_index = index;
            break;
        }
    }
    if (selected_index < 0 && active_work_list_->count() > 0) {
        selected_index = active_work_list_->count() - 1;
    }
    active_work_list_->setCurrentRow(selected_index);
    showSelectedWork();
}

void ProductResultPanel::showSelectedWork() {
    const auto index = active_work_list_->currentRow();
    if (index < 0 || index >= active_products_.size()) {
        tracking_status_->setText(QStringLiteral("위치 · 확인 중  |  경로 · 확인 중"));
        setCurrentProduct({});
        return;
    }
    const auto& product = active_products_[index];
    tracking_status_->setText(TrackingText(product.work_id, product.destination, processes_));
    tracking_status_->setToolTip(tracking_status_->text());
    setCurrentProduct(product);
}

void ProductResultPanel::setCurrentProduct(const CurrentProduct& product) {
    const bool work_changed = current_work_id_ != product.work_id;
    const bool image_changed = current_image_path_ != product.image_path;
    if (work_changed) {
        current_work_id_ = product.work_id;
    }
    if (work_changed || image_changed) {
        current_image_path_ = product.image_path;
        if (active_image_reply_ != nullptr)
            active_image_reply_->abort();
        setImagePlaceholder(QStringLiteral("상품 이미지 대기 중"));
    }

    switch (product.recognition_state) {
        case ProductRecognitionState::Waiting:
            recognition_status_->setText(QStringLiteral("상품 데이터 대기 중"));
            recognition_status_->setStyleSheet(StatusPillStyle(kSurfaceColor, kPrimaryTextColor, kBorderColor));
            break;
        case ProductRecognitionState::Processing:
            recognition_status_->setText(QStringLiteral("인식 중"));
            recognition_status_->setStyleSheet(StatusPillStyle("#3a3000", "#cca700", "#6b5d00"));
            break;
        case ProductRecognitionState::Recognized:
            recognition_status_->setText(QStringLiteral("인식 성공"));
            recognition_status_->setStyleSheet(StatusPillStyle("#1f3325", "#89d185", "#385a40"));
            break;
        case ProductRecognitionState::RecognitionFailed:
            recognition_status_->setText(QStringLiteral("인식 실패"));
            recognition_status_->setStyleSheet(StatusPillStyle("#3b1f22", "#f14c4c", "#6e2b2f"));
            break;
        case ProductRecognitionState::MissingData:
            recognition_status_->setText(QStringLiteral("데이터 누락"));
            recognition_status_->setStyleSheet(StatusPillStyle("#3a3000", "#cca700", "#6b5d00"));
            break;
    }

    processing_status_->setText(ProcessingText(product.processing_result));
    switch (product.processing_result) {
        case ProductProcessingResult::Pending:
            processing_status_->setStyleSheet(StatusPillStyle(kSurfaceColor, kPrimaryTextColor, kBorderColor));
            break;
        case ProductProcessingResult::Processing:
            processing_status_->setStyleSheet(StatusPillStyle("#182c3a", "#4daafc", "#264f78"));
            break;
        case ProductProcessingResult::Success:
            processing_status_->setStyleSheet(StatusPillStyle("#1f3325", "#89d185", "#385a40"));
            break;
        case ProductProcessingResult::Failed:
            processing_status_->setStyleSheet(StatusPillStyle("#3b1f22", "#f14c4c", "#6e2b2f"));
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

    if ((work_changed || image_changed) && !product.image_path.isEmpty()) {
        loadImage(product);
    }
}

void ProductResultPanel::setValue(QLabel* label, const QString& value) {
    label->setText(value.isEmpty() ? QStringLiteral("데이터 없음") : value);
    label->setToolTip(value);
    label->setStyleSheet(value.isEmpty() ? "color:#cca700;font-size:10px;font-weight:700;"
                                         : "color:#e7eef3;font-size:10px;font-weight:600;");
}

void ProductResultPanel::setImagePlaceholder(const QString& text, bool is_error) {
    source_image_ = {};
    image_label_->clear();
    image_label_->setText(text);
    image_label_->setToolTip({});
    image_label_->setStyleSheet(is_error
                                    ? "background:#3b1f22;color:#f14c4c;border:1px solid #6e2b2f;border-radius:4px;"
                                    : "background:#141d26;color:#91a3b0;border:1px solid #24313d;border-radius:6px;");
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
        setImagePlaceholder(QStringLiteral("이미지 경로를 확인할 수 없습니다"), true);
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
            setImagePlaceholder(http_status == 404 ? QStringLiteral("이미지가 만료되었거나 존재하지 않습니다")
                                                   : QStringLiteral("이미지를 불러오지 못했습니다"),
                                true);
            image_label_->setToolTip(detail);
            return;
        }
        const auto payload = reply->readAll();
        QPixmap image;
        if (payload.size() > kMaximumProductImageBytes || !image.loadFromData(payload)) {
            const QString detail = QStringLiteral("이미지 형식 오류: %1").arg(reply->url().toString());
            qWarning().noquote() << "[control-center][http] image decode failed:" << detail;
            setImagePlaceholder(QStringLiteral("이미지 형식을 확인할 수 없습니다"), true);
            image_label_->setToolTip(detail);
            return;
        }
        image_label_->setText({});
        image_label_->setStyleSheet("background:#141d26;border:1px solid #24313d;border-radius:6px;");
        source_image_ = image;
        updateImagePixmap();
        image_label_->setToolTip({});
    });
}

void ProductResultPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateImagePixmap();
}

void ProductResultPanel::updateImagePixmap() {
    if (source_image_.isNull() || image_label_ == nullptr) {
        return;
    }
    const auto target_size = image_label_->contentsRect().size();
    if (target_size.width() <= 1 || target_size.height() <= 1) {
        return;
    }
    image_label_->setPixmap(source_image_.scaled(target_size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

}  // namespace logistics::control_center
