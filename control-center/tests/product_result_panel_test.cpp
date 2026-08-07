#include "logistics/control_center/product_result_panel.hpp"

#include <QApplication>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLabel>
#include <QListWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QWidget>
#include <cassert>

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

void AssertUsableHorizontalContent(logistics::control_center::ProductResultPanel& panel, const QSize& size) {
    panel.resize(size);
    panel.show();
    QApplication::processEvents();

    auto* image = panel.findChild<QLabel*>(QStringLiteral("productImage"));
    auto* work_list = panel.findChild<QListWidget*>(QStringLiteral("activeWorkList"));
    auto* metadata = panel.findChild<QWidget*>(QStringLiteral("productMetadata"));
    auto* status_row = panel.findChild<QWidget*>(QStringLiteral("productStatusRow"));
    assert(image != nullptr);
    assert(work_list != nullptr);
    assert(metadata != nullptr);
    assert(status_row != nullptr);
    assert(panel.size() == size);

    const auto image_rect = PanelRect(panel, *image);
    const auto work_list_rect = PanelRect(panel, *work_list);
    const auto metadata_rect = PanelRect(panel, *metadata);
    const auto status_rect = PanelRect(panel, *status_row);
    for (const auto& rect : { work_list_rect, image_rect, metadata_rect, status_rect }) {
        assert(!rect.isEmpty());
        assert(panel.rect().contains(rect));
    }
    assert(work_list_rect.right() < image_rect.left());
    assert(image_rect.right() < metadata_rect.left());
    assert(image_rect.width() > metadata_rect.width());
    const auto ratio_error = image_rect.width() * 2 - metadata_rect.width() * 3;
    assert(ratio_error >= -3 && ratio_error <= 3);
}

}  // namespace

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::ProductResultPanel panel(QUrl(QStringLiteral("http://127.0.0.1/")));
    int empty_value_count = 0;
    for (const auto* label : panel.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("데이터 없음")) {
            assert(label->styleSheet().contains(QStringLiteral("color:#cca700")));
            ++empty_value_count;
        }
    }
    assert(empty_value_count == 7);

    logistics::control_center::CurrentProduct product;
    product.work_id = QStringLiteral("work-20260804-very-long-identifier");
    product.barcode = QStringLiteral("880123456789012345678901234567890");
    product.product_id = QStringLiteral("product-very-long-identifier-001");
    product.product_name = QStringLiteral("Long product name that must stay available in full");
    product.destination = QStringLiteral("outbound-dispatch-zone-northwest");
    product.confidence = 0.987;
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

    auto second_product = product;
    second_product.work_id = QStringLiteral("work-second");
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
    assert(work_list != nullptr && work_list->count() == 2);
    assert(tracking != nullptr);
    work_list->setCurrentRow(0);
    assert(tracking->text().contains(QStringLiteral("출발 A")));
    assert(tracking->text().contains(QStringLiteral("도착 A")));
    line_tracer.work_completed = true;
    product.processing_result = logistics::control_center::ProductProcessingResult::Success;
    second_product.processing_result = logistics::control_center::ProductProcessingResult::Success;
    panel.setActiveWorks({ product, second_product }, { line_tracer });
    assert(work_list->count() == 0);

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
