#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFrame>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QRegularExpression>
#include <QSize>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QWidget>
#include <algorithm>
#include <cstdio>

#include "logistics/control_center/factory_top_view.hpp"
#include "logistics/control_center/main_window.hpp"
#include "logistics/control_center/mqtt_client.hpp"
#include "logistics/control_center/operational_log_panel.hpp"
#include "logistics/control_center/process_control_panel.hpp"

namespace {

bool LayoutCheck(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "main_window_layout_test: %s\n", message);
    }
    return condition;
}

bool WriteChannelConfig(const QString& path, int channel_count) {
    QFile config(path);
    if (!config.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QByteArray contents =
        "[mqtt]\nhost=127.0.0.1\nport=1883\n"
        "[http]\nimage_base_url=http://127.0.0.1:1/\n"
        "[rtsp]\nchannel_count=" +
        QByteArray::number(channel_count) + "\nonvif_metadata_enabled=false\n";
    for (int channel = 1; channel <= channel_count; ++channel) {
        contents += "channel_" + QByteArray::number(channel) + "_url=rtsp://127.0.0.1:1/channel" +
                    QByteArray::number(channel) + "\n";
    }
    return config.write(contents) == contents.size();
}

bool CheckVideoCells(logistics::control_center::MainWindow& window, QWidget& video, const QList<QWidget*>& cells) {
    QList<QRect> cell_rects;
    for (auto* cell : cells) {
        const QRect rect(cell->mapTo(&video, QPoint{}), cell->size());
        if (!LayoutCheck(cell->isVisible(), "configured video cell is hidden") ||
            !LayoutCheck(!rect.isEmpty(), "configured video cell has empty geometry") ||
            !LayoutCheck(video.rect().contains(rect), "configured video cell is clipped by videoWorkspace")) {
            return false;
        }
        cell_rects.append(rect);
    }
    for (qsizetype left = 0; left < cell_rects.size(); ++left) {
        for (qsizetype right = left + 1; right < cell_rects.size(); ++right) {
            if (!LayoutCheck(!cell_rects[left].intersects(cell_rects[right]), "configured video cells overlap")) {
                return false;
            }
        }
    }
    return LayoutCheck(
        window.centralWidget()->rect().contains(QRect(video.mapTo(window.centralWidget(), QPoint{}), video.size())),
        "videoWorkspace is outside centralWidget");
}

bool CheckConfiguredChannelGrid(QApplication& application, int channel_count) {
    QTemporaryDir directory;
    if (!LayoutCheck(directory.isValid(), "channel-grid temporary directory is invalid")) {
        return false;
    }
    const auto config_path = directory.filePath(QStringLiteral("control-centor.ini"));
    if (!LayoutCheck(WriteChannelConfig(config_path, channel_count), "could not write complete channel config")) {
        return false;
    }
    qputenv("LOGISTICS_CONTROL_CENTER_CONFIG", config_path.toUtf8());

    logistics::control_center::MainWindow window;
    auto* video = window.findChild<QWidget*>(QStringLiteral("videoWorkspace"));
    auto* factory = window.findChild<QWidget*>(QStringLiteral("factoryTopView"));
    if (!LayoutCheck(video != nullptr && factory != nullptr, "configured workspace widgets are missing")) {
        return false;
    }
    QList<QWidget*> cells;
    for (int channel = 1; channel <= channel_count; ++channel) {
        auto* cell = window.findChild<QWidget*>(QStringLiteral("videoChannel%1").arg(channel));
        if (!LayoutCheck(cell != nullptr, "configured video cell is missing")) {
            return false;
        }
        cells.append(cell);
    }

    for (const auto size : { QSize(1280, 720), QSize(1600, 900) }) {
        window.resize(size);
        window.show();
        application.processEvents();
        if (qAbs(video->width() - factory->width()) > 2) {
            std::fprintf(stderr, "main_window_layout_test: %d channels at %dx%d produced %d/%d workspace widths\n",
                         channel_count, size.width(), size.height(), video->width(), factory->width());
            return false;
        }
        if (!LayoutCheck(window.size() == size, "configured window did not keep the requested geometry") ||
            !LayoutCheck(factory->isVisible(), "factoryTopView is hidden for configured channels") ||
            !CheckVideoCells(window, *video, cells)) {
            return false;
        }

        auto* focused = cells.back();
        QMouseEvent focus(QEvent::MouseButtonRelease, QPointF(4, 4), QPointF(4, 4), QPointF(4, 4), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(focused, &focus);
        application.processEvents();
        for (auto* cell : cells) {
            if (!LayoutCheck(cell->isVisible() == (cell == focused),
                             "configured focus did not isolate the selected channel")) {
                return false;
            }
        }
        const QRect focused_rect(focused->mapTo(video, QPoint{}), focused->size());
        if (!LayoutCheck(factory->isVisible(), "factoryTopView is hidden by configured channel focus") ||
            !LayoutCheck(!focused_rect.isEmpty() && video->rect().contains(focused_rect),
                         "focused configured channel is clipped")) {
            return false;
        }

        QApplication::sendEvent(focused, &focus);
        application.processEvents();
        if (!CheckVideoCells(window, *video, cells)) {
            return false;
        }
    }
    return true;
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
        if (qobject_cast<QStackedLayout*>(widget->layout()) != nullptr &&
            !widget->findChildren<QLabel*>(QRegularExpression(QStringLiteral("channelStatus[1-4]"))).isEmpty()) {
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

    auto* severity_filter = window.findChild<QComboBox*>(QStringLiteral("logSeverityFilter"));
    auto* log_table = window.findChild<QTableWidget*>(QStringLiteral("operationalLogTable"));
    if (!check(severity_filter != nullptr && severity_filter->view()->styleSheet().isEmpty(),
               "operational log popup overrides the shared item-view style") ||
        !check(log_table != nullptr && log_table->palette().color(QPalette::Highlight) == QColor("#264f78"),
               "operational log selection does not use the shared highlight")) {
        return 1;
    }
    auto* log_panel = static_cast<logistics::control_center::OperationalLogPanel*>(
        window.findChild<QWidget*>(QStringLiteral("operationalLogPanel")));
    if (!check(log_panel != nullptr, "operational log panel is missing")) {
        return 1;
    }
    logistics::control_center::OperationalLogState themed_log;
    themed_log.appendLocal(logistics::control_center::OperationalLogSeverity::Info, QStringLiteral("central-server"),
                           QStringLiteral("통신"), QStringLiteral("THEME_TEST"), QStringLiteral("공유 테마 확인"));
    log_panel->setState(themed_log);
    log_table->cellDoubleClicked(0, 3);
    application.processEvents();
    auto* detail_dialog = log_panel->findChild<QDialog*>(QStringLiteral("operationalLogDetailDialog"));
    if (!check(detail_dialog != nullptr && detail_dialog->isVisible() && detail_dialog->styleSheet().isEmpty(),
               "operational log detail overrides the shared dialog style") ||
        !check(detail_dialog->palette().color(QPalette::Window) == QColor("#181818"),
               "operational log detail does not receive the shared dialog background")) {
        return 1;
    }
    detail_dialog->close();
    application.processEvents();

    auto* factory =
        window.findChild<logistics::control_center::FactoryTopViewWidget*>(QStringLiteral("factoryTopView"));
    auto* video = window.findChild<QWidget*>(QStringLiteral("videoWorkspace"));
    auto* process_status = window.findChild<QWidget*>(QStringLiteral("processStatusSection"));
    auto* product = window.findChild<QWidget*>(QStringLiteral("productResultPanel"));
    auto* log = window.findChild<QWidget*>(QStringLiteral("operationalLogPanel"));
    auto* process_control =
        window.findChild<logistics::control_center::ProcessControlPanel*>(QStringLiteral("processControlPanel"));
    auto* app_header = window.findChild<QWidget*>(QStringLiteral("appHeader"));
    if (!check(window.size() == QSize(1280, 720), "offscreen window did not keep 1280x720") ||
        !check(factory != nullptr && factory->isVisible(), "factoryTopView is not visible") ||
        !check(video != nullptr && video->isVisible(), "videoWorkspace is not visible") ||
        !check(process_status != nullptr && process_status->isVisible(), "processStatusSection is not visible") ||
        !check(product != nullptr && product->isVisible(), "productResultPanel is not visible") ||
        !check(log != nullptr && log->isVisible(), "operationalLogPanel is not visible") ||
        !check(process_control != nullptr && process_control->isVisible(), "processControlPanel is not visible") ||
        !check(app_header != nullptr && app_header->isAncestorOf(process_control),
               "processControlPanel is not contained by appHeader") ||
        !check(app_header->minimumHeight() == 76 && app_header->maximumHeight() == 92,
               "appHeader height range is not 76-92")) {
        return 2;
    }

    auto* mqtt_client = window.findChild<logistics::control_center::MqttClient*>();
    if (!check(mqtt_client != nullptr, "MqttClient is missing")) {
        return 2;
    }
    const auto vision_color_before = factory->nodeColor(QStringLiteral("vision"));
    const auto vision_opacity_before = factory->nodeOpacity(QStringLiteral("vision"));
    mqtt_client->messageReceived(
        QStringLiteral("device/PI-VISION-01/status"),
        { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
          { QStringLiteral("messageId"), QStringLiteral("LAYOUT-VISION-LIVE") },
          { QStringLiteral("messageType"), QStringLiteral("DEVICE_STATUS") },
          { QStringLiteral("sourceId"), QStringLiteral("PI-VISION-01") },
          { QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
          { QStringLiteral("data"),
            QJsonObject{ { QStringLiteral("status"), QStringLiteral("ONLINE") },
                         { QStringLiteral("currentState"), QStringLiteral("VISION_PROCESSING") },
                         { QStringLiteral("jobId"), QStringLiteral("WORK-LAYOUT-LIVE") } } } });
    application.processEvents();
    if (!check(factory->nodeColor(QStringLiteral("vision")) != vision_color_before ||
                   factory->nodeOpacity(QStringLiteral("vision")) != vision_opacity_before,
               "live MQTT device status did not refresh factoryTopView")) {
        return 2;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto emit_device_status = [&](const QString& message_id, const QString& source_id,
                                        const QString& current_state, int timestamp_offset_ms) {
        mqtt_client->messageReceived(
            QStringLiteral("device/%1/status").arg(source_id),
            { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
              { QStringLiteral("messageId"), message_id },
              { QStringLiteral("messageType"), QStringLiteral("DEVICE_STATUS") },
              { QStringLiteral("sourceId"), source_id },
              { QStringLiteral("timestamp"), now.addMSecs(timestamp_offset_ms).toString(Qt::ISODateWithMs) },
              { QStringLiteral("data"),
                QJsonObject{ { QStringLiteral("status"), QStringLiteral("ONLINE") },
                             { QStringLiteral("currentState"), current_state },
                             { QStringLiteral("jobId"), QStringLiteral("WORK-LAYOUT-LIVE") } } } });
    };
    emit_device_status(QStringLiteral("LAYOUT-INPUT-LIVE"), QStringLiteral("PI-INPUT-01"), QStringLiteral("RUNNING"),
                       1);
    emit_device_status(QStringLiteral("LAYOUT-GRIPPER-LIVE"), QStringLiteral("PI-GRIPPER-01"),
                       QStringLiteral("TRANSFERRING"), 2);
    mqtt_client->messageReceived(
        QStringLiteral("logistics/event"),
        { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
          { QStringLiteral("messageId"), QStringLiteral("LAYOUT-DESTINATION-LIVE") },
          { QStringLiteral("messageType"), QStringLiteral("DESTINATION_SET") },
          { QStringLiteral("sourceId"), QStringLiteral("central-server") },
          { QStringLiteral("timestamp"), now.addMSecs(3).toString(Qt::ISODateWithMs) },
          { QStringLiteral("data"), QJsonObject{ { QStringLiteral("workId"), QStringLiteral("WORK-LAYOUT-LIVE") },
                                                 { QStringLiteral("destination"), QStringLiteral("2") } } } });
    emit_device_status(QStringLiteral("LAYOUT-SORTING-LIVE"), QStringLiteral("PI-SORTING-01"),
                       QStringLiteral("SORTING"), 4);
    emit_device_status(QStringLiteral("LAYOUT-LINETRACER-LIVE"), QStringLiteral("PI-LT-01"),
                       QStringLiteral("DELIVERING"), 5);
    application.processEvents();

    const auto input_before_tick = factory->boxPosition(QStringLiteral("input"));
    const auto sorting_before_tick = factory->boxPosition(QStringLiteral("sorting"));
    const auto line_before_tick = factory->boxPosition(QStringLiteral("linetracer"));
    factory->advanceAnimationsForTest();
    if (!check(factory->boxPosition(QStringLiteral("input")) != input_before_tick,
               "input node was not moving before global emergency stop") ||
        !check(factory->boxPosition(QStringLiteral("sorting")) != sorting_before_tick,
               "sorting node was not moving before global emergency stop") ||
        !check(factory->boxPosition(QStringLiteral("linetracer")) != line_before_tick,
               "line-tracer node was not moving before global emergency stop")) {
        return 2;
    }

    mqtt_client->messageReceived(QStringLiteral("logistics/emergency-stop"),
                                 { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
                                   { QStringLiteral("messageId"), QStringLiteral("LAYOUT-GLOBAL-EMERGENCY-STOP") },
                                   { QStringLiteral("messageType"), QStringLiteral("EMERGENCY_STOP") },
                                   { QStringLiteral("sourceId"), QStringLiteral("central-server") },
                                   { QStringLiteral("timestamp"), now.addMSecs(100).toString(Qt::ISODateWithMs) },
                                   { QStringLiteral("data"), QJsonObject{} } });
    application.processEvents();
    const auto stopped_input_position = factory->boxPosition(QStringLiteral("input"));
    const auto stopped_sorting_position = factory->boxPosition(QStringLiteral("sorting"));
    const auto stopped_line_position = factory->boxPosition(QStringLiteral("linetracer"));
    factory->advanceAnimationsForTest();
    factory->advanceAnimationsForTest();
    for (const auto& key : { QStringLiteral("input"), QStringLiteral("vision"), QStringLiteral("gripper"),
                             QStringLiteral("sorting"), QStringLiteral("linetracer") }) {
        if (!check(factory->nodeColor(key) == QColor(QStringLiteral("#f14c4c")),
                   "global emergency stop did not turn a connected factory node red")) {
            return 2;
        }
    }
    if (!check(factory->boxPosition(QStringLiteral("input")) == stopped_input_position,
               "input node moved after global emergency stop") ||
        !check(factory->boxPosition(QStringLiteral("sorting")) == stopped_sorting_position,
               "sorting node moved after global emergency stop") ||
        !check(factory->boxPosition(QStringLiteral("linetracer")) == stopped_line_position,
               "line-tracer node moved after global emergency stop")) {
        return 2;
    }

    auto* central = window.centralWidget();
    const auto central_rect = [central](const QWidget* widget) {
        return QRect(widget->mapTo(central, QPoint{}), widget->size());
    };
    const auto factory_rect = central_rect(factory);
    const auto video_rect = central_rect(video);
    const auto process_status_rect = central_rect(process_status);
    const auto product_rect = central_rect(product);
    const auto log_rect = central_rect(log);
    const auto process_control_rect = central_rect(process_control);
    for (const auto& [rect, message] : {
             std::pair{ factory_rect, "factoryTopView is empty or outside centralWidget" },
             std::pair{ video_rect, "videoWorkspace is empty or outside centralWidget" },
             std::pair{ process_status_rect, "processStatusSection is empty or outside centralWidget" },
             std::pair{ product_rect, "productResultPanel is empty or outside centralWidget" },
             std::pair{ log_rect, "operationalLogPanel is empty or outside centralWidget" },
             std::pair{ process_control_rect, "processControlPanel is empty or outside centralWidget" },
         }) {
        if (!check(!rect.isEmpty() && central->rect().contains(rect), message)) {
            return 3;
        }
    }
    for (const auto* cell : video_cells) {
        const QRect rect(cell->mapTo(video, QPoint{}), cell->size());
        if (!check(cell->minimumSize() == QSize(240, 135), "video cell minimum is not 240x135") ||
            !check(cell->width() >= 240 && cell->height() >= 135, "video cell is smaller than 240x135") ||
            !check(video->rect().contains(rect), "video cell is clipped by videoWorkspace")) {
            return 4;
        }
    }
    auto* channel_one = window.findChild<QWidget*>(QStringLiteral("videoChannel1"));
    auto* channel_two = window.findChild<QWidget*>(QStringLiteral("videoChannel2"));
    auto* channel_three = window.findChild<QWidget*>(QStringLiteral("videoChannel3"));
    auto* channel_four = window.findChild<QWidget*>(QStringLiteral("videoChannel4"));
    if (!check(channel_one != nullptr && channel_two != nullptr && channel_three != nullptr && channel_four != nullptr,
               "named video channel cells are missing")) {
        return 4;
    }
    QMouseEvent focus(QEvent::MouseButtonRelease, QPointF(4, 4), QPointF(4, 4), QPointF(4, 4), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(channel_two, &focus);
    application.processEvents();
    if (!check(channel_two->isVisible(), "focused channel is not visible") ||
        !check(!channel_one->isVisible() && !channel_three->isVisible() && !channel_four->isVisible(),
               "unfocused channels remain visible") ||
        !check(factory->isVisible(), "factoryTopView is hidden while a channel is focused")) {
        return 4;
    }
    QApplication::sendEvent(channel_two, &focus);
    application.processEvents();
    if (!check(channel_one->isVisible() && channel_three->isVisible() && channel_four->isVisible(),
               "all channels do not return after toggling focus")) {
        return 4;
    }
    const auto overlaps_vertically = [](const QRect& left, const QRect& right) {
        return left.top() <= right.bottom() && right.top() <= left.bottom();
    };
    const auto overlaps_horizontally = [](const QRect& left, const QRect& right) {
        return left.left() <= right.right() && right.left() <= left.right();
    };
    if (!check(overlaps_vertically(factory_rect, video_rect),
               "factory and video workspaces do not overlap vertically") ||
        !check(!overlaps_horizontally(factory_rect, video_rect), "factory and video workspaces overlap horizontally") ||
        !check(overlaps_vertically(product_rect, log_rect), "product and log panels do not overlap vertically") ||
        !check(!overlaps_horizontally(product_rect, log_rect), "product and log panels overlap horizontally")) {
        return 5;
    }

    QFrame* vision_card = nullptr;
    QFrame* input_card = nullptr;
    for (auto* card : window.findChildren<QFrame*>(QStringLiteral("processUnitCard"))) {
        const auto target = card->property("controlTargetDeviceId").toString();
        if (target == QStringLiteral("PI-VISION-01")) {
            vision_card = card;
        } else if (target == QStringLiteral("PI-INPUT-01")) {
            input_card = card;
        }
    }
    if (!check(vision_card != nullptr && input_card != nullptr, "selection test cards are missing")) {
        return 6;
    }
    QMouseEvent select_vision(QEvent::MouseButtonRelease, QPointF(4, 4), QPointF(4, 4), QPointF(4, 4), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vision_card, &select_vision);
    if (!check(factory->selectedDeviceId() == QStringLiteral("PI-VISION-01"),
               "card selection did not propagate to factory map") ||
        !check(process_control->selectedTargetDeviceId() == QStringLiteral("PI-VISION-01"),
               "card selection did not propagate to process controls")) {
        return 7;
    }

    factory->selectProcessForTest(QStringLiteral("input"));
    if (!check(input_card->property("selectedControlTarget").toBool(),
               "factory selection did not propagate to process card") ||
        !check(process_control->selectedTargetDeviceId() == QStringLiteral("PI-INPUT-01"),
               "factory selection did not propagate to process controls")) {
        return 8;
    }

    window.hide();
    application.processEvents();
    for (const int channel_count : { 1, 5, 9, 16 }) {
        if (!CheckConfiguredChannelGrid(application, channel_count)) {
            return 9;
        }
    }
    return 0;
}
