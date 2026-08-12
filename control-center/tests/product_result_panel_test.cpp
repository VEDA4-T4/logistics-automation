#include "logistics/control_center/product_result_panel.hpp"

#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QWidget>
#include <cassert>
#include <cstdlib>

namespace {

QRect PanelRect(const QWidget& panel, const QWidget& widget) {
    return { widget.mapTo(&panel, QPoint{}), widget.size() };
}

void AssertHasFullValueToolTip(const QWidget& parent, const QString& value) {
    const auto labels = parent.findChildren<QLabel*>();
    for (const auto* label : labels) {
        if (label->text() == value) {
            assert(label->toolTip().contains(value));
            assert(label->styleSheet().contains(QStringLiteral("color:#e7eef3")));
            return;
        }
    }
    assert(false);
}

QLabel* FindLabel(const QWidget& parent, const QString& text) {
    for (auto* label : parent.findChildren<QLabel*>()) {
        if (label->text() == text) {
            return label;
        }
    }
    return nullptr;
}

void AssertUsableHorizontalContent(logistics::control_center::ProductResultPanel& panel, const QSize& size) {
    panel.resize(size);
    panel.show();
    QApplication::processEvents();

    auto* image = panel.findChild<QLabel*>(QStringLiteral("productImage"));
    auto* work_list = panel.findChild<QListWidget*>(QStringLiteral("activeWorkList"));
    auto* metadata = panel.findChild<QWidget*>(QStringLiteral("productMetadataColumn"));
    auto* info_card = panel.findChild<QWidget*>(QStringLiteral("productInfoCard"));
    auto* status_row = panel.findChild<QWidget*>(QStringLiteral("productStatusRow"));
    auto* work_title = panel.findChild<QLabel*>(QStringLiteral("activeWorkListTitle"));
    auto* image_title = panel.findChild<QLabel*>(QStringLiteral("productImageTitle"));
    auto* metadata_title = panel.findChild<QLabel*>(QStringLiteral("productMetadataTitle"));
    assert(image != nullptr);
    assert(work_list != nullptr);
    assert(metadata != nullptr);
    assert(info_card != nullptr);
    assert(status_row != nullptr);
    assert(work_title != nullptr && image_title != nullptr && metadata_title != nullptr);
    assert(panel.size() == size);
    assert(image_title->contentsRect().width() >= image_title->fontMetrics().horizontalAdvance(image_title->text()));

    const auto image_rect = PanelRect(panel, *image);
    const auto work_list_rect = PanelRect(panel, *work_list);
    auto metadata_rect = PanelRect(panel, *metadata);
    metadata_rect.setTop(PanelRect(panel, *info_card).top());
    const auto status_rect = PanelRect(panel, *status_row);
    for (const auto& rect : { work_list_rect, image_rect, metadata_rect, status_rect }) {
        assert(!rect.isEmpty());
        assert(panel.rect().contains(rect));
    }
    assert(work_list_rect.right() < image_rect.left());
    assert(image_rect.right() < metadata_rect.left());
    assert(std::abs(work_list_rect.top() - image_rect.top()) <= 1);
    assert(std::abs(image_rect.top() - metadata_rect.top()) <= 1);
    assert(std::abs(work_list_rect.bottom() - image_rect.bottom()) <= 1);
    assert(std::abs(image_rect.bottom() - metadata_rect.bottom()) <= 1);
    const auto work_title_rect = PanelRect(panel, *work_title);
    const auto image_title_rect = PanelRect(panel, *image_title);
    const auto metadata_title_rect = PanelRect(panel, *metadata_title);
    assert(std::abs(work_title_rect.top() - image_title_rect.top()) <= 1);
    assert(std::abs(image_title_rect.top() - metadata_title_rect.top()) <= 1);
    assert(image_rect.width() > metadata_rect.width());
    assert(image_rect.width() >= image->fontMetrics().horizontalAdvance(image->text()));
}

}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::ProductResultPanel panel(QUrl(QStringLiteral("http://127.0.0.1/")));
    int awaiting_value_count = 0;
    for (const auto* label : panel.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("—")) {
            assert(label->styleSheet().contains(QStringLiteral("color:#6e6e6e")));
            ++awaiting_value_count;
        }
    }
    assert(awaiting_value_count == 7);
    auto* work_stack = panel.findChild<QStackedLayout*>(QStringLiteral("activeWorkStack"));
    auto* work_empty_state = panel.findChild<QLabel*>(QStringLiteral("activeWorkListEmptyState"));
    auto* image_empty_state = panel.findChild<QLabel*>(QStringLiteral("productImage"));
    auto* detail_value = panel.findChild<QLabel*>(QStringLiteral("productDetailValue"));
    assert(work_stack != nullptr && work_empty_state != nullptr);
    assert(image_empty_state != nullptr && detail_value != nullptr);
    assert(work_stack->currentWidget() == work_empty_state);
    assert(work_empty_state->text() == QStringLiteral("진행 중인 작업 없음"));
    assert(image_empty_state->text() == QStringLiteral("상품 데이터 수신 대기 중"));
    assert(work_empty_state->styleSheet() == image_empty_state->styleSheet());
    assert(detail_value->text() == QStringLiteral("상품 데이터 수신 대기 중"));
    panel.resize(460, 180);
    panel.show();
    application.processEvents();
    assert(!image_empty_state->wordWrap());
    assert(image_empty_state->contentsRect().width() >=
           image_empty_state->fontMetrics().horizontalAdvance(image_empty_state->text()));

    logistics::control_center::CurrentProduct product;
    product.work_id = QStringLiteral("work-20260804-very-long-identifier");
    product.barcode = QStringLiteral("880123456789012345678901234567890");
    product.product_id = QStringLiteral("product-very-long-identifier-001");
    product.product_name = QStringLiteral("Long product name that must stay available in full");
    product.destination = QStringLiteral("outbound-dispatch-zone-northwest");
    product.confidence = 0.987;
    product.processing_result = logistics::control_center::ProductProcessingResult::Processing;
    product.updated_at = QDateTime::fromString(QStringLiteral("2026-08-04T12:34:56"), Qt::ISODate);
    panel.setCurrentProduct(product);

    assert(panel.minimumHeight() <= 180);
    AssertUsableHorizontalContent(panel, QSize(460, 210));
    AssertUsableHorizontalContent(panel, QSize(640, 260));

    const auto expected_values = {
        product.work_id,
        product.barcode,
        product.product_id,
        product.product_name,
        product.destination,
        QStringLiteral("98.7%"),
        product.updated_at.toLocalTime().toString(QStringLiteral("MM-dd HH:mm:ss")),
    };
    for (const auto& value : expected_values) {
        AssertHasFullValueToolTip(panel, value);
    }
    auto* info_card = panel.findChild<QWidget*>(QStringLiteral("productInfoCard"));
    assert(info_card != nullptr);
    int previous_value_top = -1;
    for (const auto& value : expected_values) {
        auto* label = FindLabel(panel, value);
        assert(label != nullptr && info_card->isAncestorOf(label));
        const int value_top = label->mapTo(info_card, QPoint{}).y();
        assert(value_top > previous_value_top);
        previous_value_top = value_top;
    }
    panel.resize(460, 180);
    application.processEvents();
    auto* metadata_scroll = panel.findChild<QScrollArea*>(QStringLiteral("productMetadata"));
    auto* metadata_content = panel.findChild<QWidget*>(QStringLiteral("productMetadataContent"));
    auto* detail_card = panel.findChild<QWidget*>(QStringLiteral("productDetailCard"));
    assert(metadata_scroll != nullptr && metadata_content != nullptr && detail_card != nullptr);
    assert(metadata_content->styleSheet().contains(QStringLiteral("background-color:#141d26")));
    assert(metadata_scroll->viewport()->styleSheet().contains(QStringLiteral("background-color:#141d26")));
    assert(metadata_scroll->styleSheet().contains(QStringLiteral("QScrollBar::add-line:vertical")));
    assert(metadata_scroll->styleSheet().contains(QStringLiteral("background:#1f1f1f")));
    assert(metadata_scroll->styleSheet().contains(QStringLiteral("min-height:8px")));
    assert(metadata_content->minimumHeight() == 140);
    assert(metadata_scroll->verticalScrollBar()->maximum() > 0);
    const auto info_card_geometry = info_card->geometry();
    metadata_scroll->verticalScrollBar()->setValue(metadata_scroll->verticalScrollBar()->maximum());
    application.processEvents();
    assert(metadata_scroll->verticalScrollBar()->value() == metadata_scroll->verticalScrollBar()->maximum());
    assert(metadata_scroll->verticalScrollBar()->value() > metadata_scroll->verticalScrollBar()->minimum());
    assert(info_card->geometry() == info_card_geometry);
    assert(info_card->rect().contains(QRect(metadata_scroll->mapTo(info_card, QPoint{}), metadata_scroll->size())));
    assert(detail_card->isVisible());
    assert(detail_card->height() == 46);
    assert(panel.rect().contains(QRect(detail_card->mapTo(&panel, QPoint{}), detail_card->size())));
    assert(detail_value->isVisible());
    assert(!detail_value->wordWrap());
    assert(detail_value->height() == 18);
    assert(detail_value->styleSheet().contains(QStringLiteral("font-size:9px")));
    assert(detail_card->rect().contains(QRect(detail_value->mapTo(detail_card, QPoint{}), detail_value->size())));
    for (const auto& value : expected_values) {
        const auto* label = FindLabel(panel, value);
        assert(label != nullptr && label->height() >= 16);
    }

    auto second_product = product;
    second_product.work_id = QStringLiteral("work-second");
    second_product.processing_result = logistics::control_center::ProductProcessingResult::Success;
    logistics::control_center::ProcessUnitStatus line_tracer;
    line_tracer.key = QStringLiteral("linetracer");
    line_tracer.display_name = QStringLiteral("라인트레이서");
    line_tracer.work_id = product.work_id;
    line_tracer.current_state = QStringLiteral("FOLLOWING_LINE");
    line_tracer.departure_position =
        logistics::control_center::LineTracerPositionStatus{ .area = QStringLiteral("DEPARTURE"),
                                                             .location = QStringLiteral("A") };
    line_tracer.target_position =
        logistics::control_center::LineTracerPositionStatus{ .area = QStringLiteral("DESTINATION"),
                                                             .location = QStringLiteral("A") };
    line_tracer.confirmed_position = line_tracer.departure_position;
    panel.setActiveWorks({ product, second_product }, { line_tracer });
    auto* work_list = panel.findChild<QListWidget*>(QStringLiteral("activeWorkList"));
    auto* tracking = panel.findChild<QLabel*>(QStringLiteral("workTrackingStatus"));
    assert(work_list != nullptr && work_list->count() == 1);
    assert(work_stack->currentWidget() == work_list);
    assert(tracking != nullptr);
    assert(tracking->wordWrap());
    work_list->setCurrentRow(0);
    assert(tracking->text().contains(QStringLiteral("출발 A")));
    assert(tracking->text().contains(QStringLiteral("도착 A")));
    line_tracer.work_completed = true;
    product.processing_result = logistics::control_center::ProductProcessingResult::Success;
    second_product.processing_result = logistics::control_center::ProductProcessingResult::Success;
    panel.setActiveWorks({ product, second_product }, { line_tracer });
    assert(work_list->count() == 0);
    assert(work_stack->currentWidget() == work_empty_state);
    for (const auto* label : panel.findChildren<QLabel*>()) {
        assert(label->text() != product.work_id);
        assert(label->text() != second_product.work_id);
    }

    QTcpServer image_server;
    assert(image_server.listen(QHostAddress::LocalHost));
    const auto png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");
    QByteArray request;
    QObject::connect(&image_server, &QTcpServer::newConnection, &application, [&]() {
        auto* socket = image_server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, &application, [&, socket]() {
            request.append(socket->readAll());
            if (!request.contains("\r\n\r\n")) {
                return;
            }
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " +
                          QByteArray::number(png.size()) + "\r\nConnection: close\r\n\r\n" + png);
            socket->disconnectFromHost();
        });
    });

    logistics::control_center::ProductResultPanel image_panel(
        QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(image_server.serverPort())));
    image_panel.resize(460, 210);
    image_panel.show();
    logistics::control_center::CurrentProduct imaged_product;
    imaged_product.work_id = QStringLiteral("work-with-image");
    imaged_product.image_path = QStringLiteral("product.png");
    image_panel.setCurrentProduct(imaged_product);
    auto* image = image_panel.findChild<QLabel*>(QStringLiteral("productImage"));
    assert(image != nullptr);
    QElapsedTimer image_timeout;
    image_timeout.start();
    while (image->pixmap().isNull() && image_timeout.elapsed() < 3000) {
        application.processEvents();
        QThread::msleep(1);
    }
    assert(!image->pixmap().isNull());
    assert(image->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored);
    assert(image->sizePolicy().verticalPolicy() == QSizePolicy::Ignored);
    image_panel.resize(640, 260);
    application.processEvents();
    const auto expanded_image_size = image->size();
    const auto expanded_pixmap_size = image->pixmap().size();
    image_panel.resize(400, 190);
    application.processEvents();
    assert(image->width() < expanded_image_size.width());
    assert(image->height() < expanded_image_size.height());
    assert(image->pixmap().width() <= image->contentsRect().width());
    assert(image->pixmap().height() <= image->contentsRect().height());
    assert(image->pixmap().size() != expanded_pixmap_size);

    logistics::control_center::CurrentProduct image_less_product;
    image_less_product.work_id = QStringLiteral("different-work-without-image");
    image_panel.setCurrentProduct(image_less_product);
    assert(image->pixmap().isNull());
    assert(!image->text().isEmpty());
    image_panel.resize(640, 260);
    application.processEvents();
    assert(image->pixmap().isNull());
    return 0;
}
