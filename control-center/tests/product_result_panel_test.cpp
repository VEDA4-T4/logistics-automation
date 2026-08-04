#include "logistics/control_center/product_result_panel.hpp"

#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <cassert>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    logistics::control_center::ProductResultPanel panel(QUrl(QStringLiteral("http://127.0.0.1/")));
    panel.resize(480, 180);
    panel.show();
    application.processEvents();

    auto* image = panel.findChild<QLabel*>(QStringLiteral("productImage"));
    auto* info = panel.findChild<QFrame*>(QStringLiteral("productInfoCard"));
    auto* detail = panel.findChild<QFrame*>(QStringLiteral("productDetailCard"));
    assert(image != nullptr);
    assert(info != nullptr);
    assert(detail != nullptr);
    assert(panel.minimumHeight() <= 180);
    assert(panel.size() == QSize(480, 180));

    const auto panel_rect = [&panel](const QWidget* widget) {
        return QRect(widget->mapTo(&panel, QPoint{}), widget->size());
    };
    const auto image_rect = panel_rect(image);
    const auto info_rect = panel_rect(info);
    const auto detail_rect = panel_rect(detail);
    for (const auto& rect : { image_rect, info_rect, detail_rect }) {
        assert(!rect.isEmpty());
        assert(panel.rect().contains(rect));
    }
    assert(image_rect.top() <= info_rect.bottom() && info_rect.top() <= image_rect.bottom());
    assert(image_rect.right() < info_rect.left() || info_rect.right() < image_rect.left());
    assert(info_rect.left() == detail_rect.left());
    assert(info_rect.right() == detail_rect.right());
    return 0;
}
