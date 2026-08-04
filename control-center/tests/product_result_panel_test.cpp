#include "logistics/control_center/product_result_panel.hpp"

#include <QApplication>
#include <QDateTime>
#include <QLabel>
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
    auto* metadata = panel.findChild<QWidget*>(QStringLiteral("productMetadata"));
    auto* status_row = panel.findChild<QWidget*>(QStringLiteral("productStatusRow"));
    assert(image != nullptr);
    assert(metadata != nullptr);
    assert(status_row != nullptr);
    assert(panel.size() == size);

    const auto image_rect = PanelRect(panel, *image);
    const auto metadata_rect = PanelRect(panel, *metadata);
    const auto status_rect = PanelRect(panel, *status_row);
    for (const auto& rect : { image_rect, metadata_rect, status_rect }) {
        assert(!rect.isEmpty());
        assert(panel.rect().contains(rect));
    }
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
    return 0;
}
