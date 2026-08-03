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

bool StyleRuleContains(const QString& style_sheet, QStringView selector, QStringView property) {
    const auto rule_start = style_sheet.indexOf(selector);
    const auto rule_end = style_sheet.indexOf('}', rule_start);
    return rule_start >= 0 && rule_end > rule_start &&
           QStringView(style_sheet).sliced(rule_start, rule_end - rule_start).contains(property);
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
    if (!check(StyleRuleContains(shared_theme, combo_popup, u"background:#252526"),
               "shared theme does not give combo popups a dark background") ||
        !check(StyleRuleContains(shared_theme, combo_popup, u"color:#f0f0f0"),
               "shared theme does not give combo popups a light foreground") ||
        !check(StyleRuleContains(window.styleSheet(), combo_popup, u"background:#252526"),
               "MainWindow does not apply the shared combo popup theme") ||
        !check(StyleRuleContains(window.styleSheet(), combo_popup, u"color:#f0f0f0"),
               "MainWindow does not apply the shared combo popup foreground")) {
        return 1;
    }

    auto* workspace = window.findChild<QSplitter*>(QStringLiteral("workspaceSplitter"));
    if (!check(workspace != nullptr, "workspaceSplitter is missing")) {
        return 1;
    }
    if (!check(workspace->orientation() == Qt::Vertical, "workspaceSplitter is not vertical") ||
        !check(!workspace->childrenCollapsible(), "workspaceSplitter children are collapsible")) {
        return 1;
    }

    auto* content = window.findChild<QSplitter*>(QStringLiteral("contentSplitter"));
    if (!check(content != nullptr, "contentSplitter is missing")) {
        return 1;
    }
    if (!check(content->orientation() == Qt::Horizontal, "contentSplitter is not horizontal") ||
        !check(!content->childrenCollapsible(), "contentSplitter children are collapsible")) {
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

    const auto has_supported_geometry = [&](const QSize& size) {
        window.resize(size);
        window.show();
        application.processEvents();

        if (!check(window.size() == size, "offscreen window did not keep the requested size")) {
            return false;
        }
        const auto workspace_sizes = workspace->sizes();
        if (!check(workspace_sizes.size() == 2, "workspaceSplitter does not have two panes") ||
            !check(workspace_sizes[0] >= 250, "factory overview is shorter than 250 px") ||
            !check(workspace_sizes[1] >= 360, "lower workspace is shorter than 360 px")) {
            return false;
        }

        const auto content_sizes = content->sizes();
        if (!check(content_sizes.size() == 2, "contentSplitter does not have two panes") ||
            !check(content_sizes[0] >= 320, "video workspace is narrower than 320 px") ||
            !check(content_sizes[1] >= 450, "side panel is narrower than 450 px")) {
            return false;
        }
        for (const auto* cell : video_cells) {
            if (!check(cell->minimumSize() == QSize(320, 180), "a video cell does not enforce a 320x180 minimum") ||
                !check(cell->width() >= 320 && cell->height() >= 180, "a visible video cell is smaller than 320x180")) {
                return false;
            }
        }
        return true;
    };

    if (!has_supported_geometry(QSize(1280, 720)) || !has_supported_geometry(QSize(1600, 900))) {
        return 2;
    }
}
