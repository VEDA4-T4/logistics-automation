#include <QApplication>
#include <QFile>
#include <QSize>
#include <QSplitter>
#include <QStackedLayout>
#include <QStringView>
#include <QTemporaryDir>
#include <QWidget>
#include <algorithm>
#include <cstdio>

#include "logistics/control_center/control_center_theme.hpp"
#include "logistics/control_center/main_window.hpp"

namespace {

bool StyleRuleHasDeclaration(const QString& style_sheet, QStringView selector, QStringView property,
                             QStringView value) {
    const auto rule_start = style_sheet.indexOf(selector);
    const auto rule_open = style_sheet.indexOf('{', rule_start);
    const auto rule_end = style_sheet.indexOf('}', rule_start);
    if (rule_start < 0 || rule_open < 0 || rule_end <= rule_open) {
        return false;
    }
    const auto declarations =
        QStringView(style_sheet).sliced(rule_open + 1, rule_end - rule_open - 1).split(';', Qt::SkipEmptyParts);
    return std::ranges::any_of(declarations, [property, value](QStringView declaration) {
        const auto separator = declaration.indexOf(':');
        return separator >= 0 && declaration.first(separator).trimmed() == property &&
               declaration.sliced(separator + 1).trimmed() == value;
    });
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto check = [](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "main_window_layout_test: %s\n", message);
        }
        return condition;
    };

    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory is invalid")) {
        return 1;
    }
    const auto config_path = directory.filePath(QStringLiteral("control-centor.ini"));
    QFile config(config_path);
    if (!check(config.open(QIODevice::WriteOnly | QIODevice::Text), "could not write temporary config")) {
        return 1;
    }
    config.write(
        "[mqtt]\nhost=127.0.0.1\nport=1883\n"
        "[rtsp]\nchannel_count=4\n"
        "channel_1_url=rtsp://127.0.0.1:1/channel1\n"
        "channel_2_url=rtsp://127.0.0.1:1/channel2\n"
        "channel_3_url=rtsp://127.0.0.1:1/channel3\n"
        "channel_4_url=rtsp://127.0.0.1:1/channel4\n"
        "onvif_metadata_enabled=false\n");
    config.close();
    qputenv("LOGISTICS_CONTROL_CENTER_CONFIG", config_path.toUtf8());

    logistics::control_center::MainWindow window;
    const auto shared_theme = logistics::control_center::ControlCenterStyleSheet();
    constexpr auto combo_popup = u"QComboBox QAbstractItemView";
    if (!check(StyleRuleHasDeclaration(shared_theme, combo_popup, u"background", u"#252526"),
               "shared theme does not give combo popups a dark background") ||
        !check(StyleRuleHasDeclaration(shared_theme, combo_popup, u"color", u"#f0f0f0"),
               "shared theme does not give combo popups a light foreground") ||
        !check(StyleRuleHasDeclaration(window.styleSheet(), combo_popup, u"background", u"#252526"),
               "MainWindow does not apply the shared combo popup theme") ||
        !check(StyleRuleHasDeclaration(window.styleSheet(), combo_popup, u"color", u"#f0f0f0"),
               "MainWindow does not apply the shared combo popup foreground") ||
        !check(!StyleRuleHasDeclaration(QStringLiteral("QComboBox QAbstractItemView { "
                                                       "selection-background:#252526; selection-color:#f0f0f0; }"),
                                        combo_popup, u"background", u"#252526"),
               "theme declaration check confuses selection-background with background") ||
        !check(!StyleRuleHasDeclaration(QStringLiteral("QComboBox QAbstractItemView { "
                                                       "selection-background:#252526; selection-color:#f0f0f0; }"),
                                        combo_popup, u"color", u"#f0f0f0"),
               "theme declaration check confuses selection-color with color")) {
        return 1;
    }

    auto* operations_workspace = window.findChild<QSplitter*>(QStringLiteral("operationsWorkspaceSplitter"));
    if (!check(operations_workspace != nullptr, "operationsWorkspaceSplitter is missing")) {
        return 1;
    }
    if (!check(operations_workspace->orientation() == Qt::Horizontal,
               "operationsWorkspaceSplitter is not horizontal") ||
        !check(!operations_workspace->childrenCollapsible(), "operationsWorkspaceSplitter children are collapsible")) {
        return 1;
    }

    auto* detail = window.findChild<QSplitter*>(QStringLiteral("detailSplitter"));
    if (!check(detail != nullptr, "detailSplitter is missing")) {
        return 1;
    }
    if (!check(detail->orientation() == Qt::Horizontal, "detailSplitter is not horizontal") ||
        !check(!detail->childrenCollapsible(), "detailSplitter children are collapsible")) {
        return 1;
    }

    const auto widgets = window.findChildren<QWidget*>();
    QList<QWidget*> video_cells;
    for (auto* widget : widgets) {
        const auto descendants = widget->findChildren<QWidget*>();
        if (qobject_cast<QStackedLayout*>(widget->layout()) != nullptr &&
            std::ranges::any_of(descendants,
                                [](const QWidget* child) { return child->minimumSize() == QSize(320, 180); })) {
            video_cells.append(widget);
        }
    }
    if (!check(video_cells.size() == 4, "expected four stacked video channel cells")) {
        return 1;
    }
    auto* video_viewport = video_cells.front()->parentWidget();
    if (!check(video_viewport != nullptr, "video grid viewport is missing") ||
        !check(
            std::ranges::all_of(
                video_cells, [video_viewport](const QWidget* cell) { return cell->parentWidget() == video_viewport; }),
            "video channel cells do not share the grid viewport")) {
        return 1;
    }

    window.resize(1280, 720);
    window.show();
    application.processEvents();

    auto* factory = window.findChild<QWidget*>(QStringLiteral("factoryTopView"));
    auto* video = window.findChild<QWidget*>(QStringLiteral("videoWorkspace"));
    auto* process_status = window.findChild<QWidget*>(QStringLiteral("processStatusSection"));
    auto* product = window.findChild<QWidget*>(QStringLiteral("productResultPanel"));
    auto* log = window.findChild<QWidget*>(QStringLiteral("operationalLogPanel"));
    if (!check(window.size() == QSize(1280, 720), "offscreen window did not keep 1280x720") ||
        !check(factory != nullptr && factory->isVisible(), "factoryTopView is not visible") ||
        !check(video != nullptr && video->isVisible(), "videoWorkspace is not visible") ||
        !check(process_status != nullptr && process_status->isVisible(), "processStatusSection is not visible") ||
        !check(product != nullptr && product->isVisible(), "productResultPanel is not visible") ||
        !check(log != nullptr && log->isVisible(), "operationalLogPanel is not visible")) {
        return 2;
    }

    const auto window_rect = [&window](const QWidget* widget) {
        return QRect(widget->mapTo(&window, QPoint{}), widget->size());
    };
    const auto factory_rect = window_rect(factory);
    const auto video_rect = window_rect(video);
    const auto product_rect = window_rect(product);
    const auto log_rect = window_rect(log);
    const auto overlaps_vertically = [](const QRect& left, const QRect& right) {
        return left.top() <= right.bottom() && right.top() <= left.bottom();
    };
    const auto overlaps_horizontally = [](const QRect& left, const QRect& right) {
        return left.left() <= right.right() && right.left() <= left.right();
    };
    if (!check(overlaps_vertically(factory_rect, video_rect),
               "factory and video workspaces do not overlap vertically") ||
        !check(!overlaps_horizontally(factory_rect, video_rect),
               "factory and video workspaces overlap horizontally") ||
        !check(overlaps_vertically(product_rect, log_rect), "product and log panels do not overlap vertically") ||
        !check(!overlaps_horizontally(product_rect, log_rect), "product and log panels overlap horizontally")) {
        return 3;
    }
}
