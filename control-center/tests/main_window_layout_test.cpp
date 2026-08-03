#include <QApplication>
#include <QFile>
#include <QSize>
#include <QSplitter>
#include <QTemporaryDir>
#include <QWidget>
#include <algorithm>
#include <cassert>

#include "logistics/control_center/main_window.hpp"

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QTemporaryDir directory;
    assert(directory.isValid());
    const auto config_path = directory.filePath(QStringLiteral("control-centor.ini"));
    QFile config(config_path);
    assert(config.open(QIODevice::WriteOnly | QIODevice::Text));
    config.write(
        "[mqtt]\nhost=127.0.0.1\nport=1883\n"
        "[rtsp]\nchannel_count=1\nchannel_1_url=rtsp://127.0.0.1:1/channel1\n"
        "onvif_metadata_enabled=false\n");
    config.close();
    qputenv("LOGISTICS_CONTROL_CENTER_CONFIG", config_path.toUtf8());

    logistics::control_center::MainWindow window;
    auto* workspace = window.findChild<QSplitter*>(QStringLiteral("workspaceSplitter"));
    if (workspace == nullptr) {
        return 1;
    }
    assert(workspace->orientation() == Qt::Vertical);
    assert(!workspace->childrenCollapsible());

    auto* content = window.findChild<QSplitter*>(QStringLiteral("contentSplitter"));
    assert(content != nullptr);
    assert(content->orientation() == Qt::Horizontal);
    assert(!content->childrenCollapsible());

    const auto assert_supported_geometry = [&](const QSize& size) {
        window.resize(size);
        window.show();
        application.processEvents();

        assert(window.size() == size);
        const auto workspace_sizes = workspace->sizes();
        assert(workspace_sizes.size() == 2);
        assert(workspace_sizes[0] >= 250);
        assert(workspace_sizes[1] >= 360);

        const auto content_sizes = content->sizes();
        assert(content_sizes.size() == 2);
        assert(content_sizes[0] >= 320);
        assert(content_sizes[1] >= 450);
    };

    assert_supported_geometry(QSize(1280, 720));
    assert_supported_geometry(QSize(1600, 900));

    const auto widgets = window.findChildren<QWidget*>();
    assert(
        std::ranges::any_of(widgets, [](const QWidget* widget) { return widget->minimumSize() == QSize(320, 180); }));
}
