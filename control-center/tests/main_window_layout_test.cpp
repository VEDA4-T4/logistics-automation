#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QHeaderView>
#include <QHostAddress>
#include <QJsonObject>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSize>
#include <QSplitter>
#include <QStackedLayout>
#include <QTableView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVideoSink>
#include <QWidget>
#include <cstdio>

#include "logistics/control_center/detection_overlay.hpp"
#include "logistics/control_center/factory_top_view.hpp"
#include "logistics/control_center/main_window.hpp"
#include "logistics/control_center/mqtt_client.hpp"
#include "logistics/control_center/onvif_rtsp_metadata_client.hpp"
#include "logistics/control_center/operational_log_panel.hpp"
#include "logistics/control_center/process_control_panel.hpp"

namespace {

bool LayoutCheck(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "main_window_layout_test: %s\n", message);
    }
    return condition;
}

template <typename Predicate>
bool WaitUntil(QApplication& application, Predicate predicate, int timeout_ms = 3000) {
    QElapsedTimer timeout;
    timeout.start();
    while (!predicate() && timeout.elapsed() < timeout_ms) {
        application.processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

bool CheckHistoryPaging(QApplication& application) {
    QTcpServer server;
    if (!LayoutCheck(server.listen(QHostAddress::LocalHost), "history test server could not listen")) {
        return false;
    }
    QList<QByteArray> requests;
    QTcpSocket* delayed_error_socket = nullptr;
    const auto write_response = [](QTcpSocket* socket, const QByteArray& status, const QByteArray& body) {
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    };
    QObject::connect(&server, &QTcpServer::newConnection, &application, [&]() {
        auto* socket = server.nextPendingConnection();
        QObject::connect(
            socket, &QTcpSocket::readyRead, &application,
            [&, socket, request = QByteArray{}, handled = false]() mutable {
                if (handled) {
                    return;
                }
                request.append(socket->readAll());
                if (!request.contains("\r\n\r\n")) {
                    return;
                }
                handled = true;
                requests.append(request);
                switch (requests.size()) {
                    case 1:
                        write_response(
                            socket, "200 OK",
                            R"({"count":2,"items":[{"historyId":"100.2.2","messageId":"HIST-1","eventType":"DEVICE_STATUS","sourceId":"HISTORY-A","state":"STORED","errorCode":"","severity":"","message":"","details":{},"occurredAtMs":100},{"historyId":"100.2.1","messageId":"HIST-2","eventType":"ERROR_OCCURRED","sourceId":"HISTORY-B","state":"","errorCode":"ERR-HISTORY","severity":"ERROR","message":"과거 오류","details":{},"occurredAtMs":100}],"nextCursor":"100.2.1"})");
                        break;
                    case 2:
                        delayed_error_socket = socket;
                        break;
                    case 3:
                        write_response(
                            socket, "200 OK",
                            R"({"count":2,"items":[{"historyId":"100.2.2","messageId":"HIST-1","eventType":"DEVICE_STATUS","sourceId":"HISTORY-A","state":"STORED","errorCode":"","severity":"","message":"","details":{},"occurredAtMs":100},{"historyId":"100.2.1","messageId":"HIST-2","eventType":"ERROR_OCCURRED","sourceId":"HISTORY-B","state":"","errorCode":"ERR-HISTORY","severity":"ERROR","message":"과거 오류","details":{},"occurredAtMs":100}],"nextCursor":"90.2.5"})");
                        break;
                    default:
                        write_response(
                            socket, "200 OK",
                            R"({"count":1,"items":[{"historyId":"90.2.4","messageId":"HIST-3","eventType":"WORK_CREATED","sourceId":"HISTORY-C","state":"STORED","errorCode":"","severity":"","message":"","details":{},"occurredAtMs":90}],"nextCursor":null})");
                        break;
                }
            });
    });

    QTemporaryDir directory;
    if (!LayoutCheck(directory.isValid(), "history config temporary directory is invalid")) {
        return false;
    }
    const auto config_path = directory.filePath(QStringLiteral("control-centor.ini"));
    QFile config(config_path);
    if (!LayoutCheck(config.open(QIODevice::WriteOnly | QIODevice::Text), "could not write history test config")) {
        return false;
    }
    const auto contents = QByteArray("[mqtt]\nhost=127.0.0.1\nport=1\n[http]\nimage_base_url=http://127.0.0.1:") +
                          QByteArray::number(server.serverPort()) +
                          "/\nbearer_token=local-history-token\n[rtsp]\nchannel_count=1\n"
                          "channel_1_url=rtsp://127.0.0.1:1/channel1\nonvif_metadata_enabled=false\n";
    if (!LayoutCheck(config.write(contents) == contents.size(), "history test config write was incomplete")) {
        return false;
    }
    config.close();
    qputenv("LOGISTICS_CONTROL_CENTER_CONFIG", config_path.toUtf8());
    const auto danger_zone_config_path = directory.filePath(QStringLiteral("danger-zones.ini"));
    qputenv("LOGISTICS_CONTROL_CENTER_DANGER_ZONE_CONFIG", danger_zone_config_path.toUtf8());
    logistics::control_center::MainWindow window;
    auto* table = window.findChild<QTableView*>(QStringLiteral("operationalLogTable"));
    auto* log_panel_widget = window.findChild<QWidget*>(QStringLiteral("operationalLogPanel"));
    auto* log_panel = static_cast<logistics::control_center::OperationalLogPanel*>(log_panel_widget);
    if (!LayoutCheck(table != nullptr && log_panel != nullptr, "history test log panel is missing") ||
        !LayoutCheck(WaitUntil(application, [&]() { return requests.size() == 1 && table->model()->rowCount() >= 2; }),
                     "first history page was not loaded") ||
        !LayoutCheck(requests.front().contains("Authorization: Bearer local-history-token"),
                     "history authorization header is missing") ||
        !LayoutCheck(requests.front().contains("GET /api/v1/history?limit=100 HTTP/1.1"),
                     "initial history request did not use limit=100")) {
        return false;
    }

    bool found_first_page = false;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        found_first_page =
            found_first_page || table->model()->index(row, 2).data().toString() == QStringLiteral("HISTORY-A");
    }
    if (!LayoutCheck(found_first_page && log_panel->canLoadOlderEntries(), "first history page cannot fetch more")) {
        return false;
    }
    log_panel->requestOlderEntries();
    if (!LayoutCheck(WaitUntil(application, [&]() { return requests.size() == 2 && delayed_error_socket != nullptr; }),
                     "second history page was not requested")) {
        return false;
    }
    log_panel->requestOlderEntries();
    application.processEvents();
    if (!LayoutCheck(requests.size() == 2 && !log_panel->canLoadOlderEntries(),
                     "history request was not kept to one in-flight request")) {
        return false;
    }
    write_response(delayed_error_socket, "503 Service Unavailable",
                   R"({"error":"TEMPORARY_FAILURE","message":"retry"})");
    delayed_error_socket = nullptr;
    if (!LayoutCheck(WaitUntil(application, [&]() { return log_panel->canLoadOlderEntries(); }),
                     "failed history page did not become retryable")) {
        return false;
    }
    log_panel->requestOlderEntries();
    bool found_second_page = false;
    if (!LayoutCheck(WaitUntil(application,
                               [&]() {
                                   for (int row = 0; row < table->model()->rowCount(); ++row) {
                                       if (table->model()->index(row, 2).data().toString() ==
                                           QStringLiteral("HISTORY-C")) {
                                           return true;
                                       }
                                   }
                                   return false;
                               }),
                     "second history page was not appended")) {
        return false;
    }
    if (!LayoutCheck(requests.size() == 4, "duplicate history page was not skipped automatically") ||
        !LayoutCheck(
            requests[1].contains("limit=100&cursor=100.2.1") && requests[2].contains("limit=100&cursor=100.2.1"),
            "history retry did not preserve its cursor") ||
        !LayoutCheck(requests[3].contains("limit=100&cursor=90.2.5"),
                     "duplicate history page did not advance to its next cursor")) {
        return false;
    }
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        found_second_page =
            found_second_page || table->model()->index(row, 2).data().toString() == QStringLiteral("HISTORY-C");
    }
    bool retained_first_page = false;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        retained_first_page =
            retained_first_page || table->model()->index(row, 2).data().toString() == QStringLiteral("HISTORY-A");
    }
    return LayoutCheck(found_second_page && retained_first_page && !log_panel->canLoadOlderEntries(),
                       "history paging did not retain loaded rows or stop after null nextCursor");
}

bool WriteSingleChannelConfig(const QString& path) {
    QFile config(path);
    if (!config.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    const QByteArray contents =
        "[mqtt]\nhost=127.0.0.1\nport=1\n"
        "[http]\nimage_base_url=http://127.0.0.1:1/\n"
        "[rtsp]\nchannel_count=4\nonvif_metadata_enabled=true\n"
        "channel_1_url=rtsp://127.0.0.1:1/channel1\n"
        "channel_2_url=rtsp://127.0.0.1:1/channel2\n"
        "channel_3_url=rtsp://127.0.0.1:1/channel3\n"
        "channel_4_url=rtsp://127.0.0.1:1/channel4\n";
    return config.write(contents) == contents.size();
}

bool CheckVideoCells(logistics::control_center::MainWindow& window, QWidget& video, const QList<QWidget*>& cells) {
    QList<QRect> cell_rects;
    for (auto* cell : cells) {
        const QRect rect(cell->mapTo(&video, QPoint{}), cell->size());
        if (!video.rect().contains(rect)) {
            std::fprintf(stderr,
                         "main_window_layout_test: video cell geometry (%d,%d %dx%d) exceeds workspace (%dx%d)\n",
                         rect.x(), rect.y(), rect.width(), rect.height(), video.width(), video.height());
        }
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

bool CheckConfiguredSingleChannel(QApplication& application) {
    QTemporaryDir directory;
    if (!LayoutCheck(directory.isValid(), "channel-grid temporary directory is invalid")) {
        return false;
    }
    const auto config_path = directory.filePath(QStringLiteral("control-centor.ini"));
    if (!LayoutCheck(WriteSingleChannelConfig(config_path), "could not write single-channel config")) {
        return false;
    }
    qputenv("LOGISTICS_CONTROL_CENTER_CONFIG", config_path.toUtf8());

    logistics::control_center::MainWindow window;
    auto* video = window.findChild<QWidget*>(QStringLiteral("videoWorkspace"));
    auto* factory = window.findChild<QWidget*>(QStringLiteral("factoryTopView"));
    if (!LayoutCheck(video != nullptr && factory != nullptr, "configured workspace widgets are missing")) {
        return false;
    }
    auto* cell = window.findChild<QWidget*>(QStringLiteral("videoChannel1"));
    if (!LayoutCheck(cell != nullptr, "configured video cell is missing") ||
        !LayoutCheck(window.findChild<QWidget*>(QStringLiteral("videoChannel2")) == nullptr,
                     "legacy channel count created an extra video cell") ||
        !LayoutCheck(window.findChildren<QMediaPlayer*>().size() == 1, "expected exactly one media player") ||
        !LayoutCheck(window.findChildren<QVideoSink*>().size() == 1, "expected exactly one detection video sink") ||
        !LayoutCheck(window.findChildren<logistics::control_center::OnvifRtspMetadataClient*>().size() == 1,
                     "expected exactly one ONVIF metadata client")) {
        return false;
    }
    const QList<QWidget*> cells{ cell };

    for (const auto size : { QSize(1280, 720), QSize(1600, 900) }) {
        window.resize(size);
        window.show();
        application.processEvents();
        const int ratio_error = qAbs(video->width() * 9 - factory->width() * 11);
        if (video->width() <= factory->width() || ratio_error > 40) {
            std::fprintf(stderr, "main_window_layout_test: single channel at %dx%d produced %d/%d workspace widths\n",
                         size.width(), size.height(), video->width(), factory->width());
            return false;
        }
        if (!LayoutCheck(window.size() == size, "configured window did not keep the requested geometry") ||
            !LayoutCheck(factory->isVisible(), "factoryTopView is hidden for configured channels") ||
            !CheckVideoCells(window, *video, cells)) {
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

#ifdef Q_OS_WIN
    qputenv("QT_QPA_PLATFORM", "windows");
#else
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    QApplication::setDesktopSettingsAware(false);
    QApplication application(argc, argv);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, true);
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
        "[mqtt]\nhost=127.0.0.1\nport=1\n"
        "[rtsp]\nchannel_count=4\n"
        "channel_1_url=rtsp://127.0.0.1:1/channel1\n"
        "channel_2_url=rtsp://127.0.0.1:1/channel2\n"
        "channel_3_url=rtsp://127.0.0.1:1/channel3\n"
        "channel_4_url=rtsp://127.0.0.1:1/channel4\n"
        "onvif_metadata_enabled=false\n");
    config.close();
    qputenv("LOGISTICS_CONTROL_CENTER_CONFIG", config_path.toUtf8());
    const auto danger_zone_config_path = directory.filePath(QStringLiteral("danger-zones.ini"));
    qputenv("LOGISTICS_CONTROL_CENTER_DANGER_ZONE_CONFIG", danger_zone_config_path.toUtf8());
    logistics::control_center::MainWindow window;

    auto* danger_toggle = window.findChild<QPushButton*>(QStringLiteral("dangerZoneToggleButton"));
    auto* danger_actions = window.findChild<QWidget*>(QStringLiteral("dangerZoneActions"));
    auto* danger_settings = window.findChild<QPushButton*>(QStringLiteral("dangerZoneSettingsButton"));
    auto* danger_visibility = window.findChild<QPushButton*>(QStringLiteral("dangerZoneVisibilityButton"));
    auto* danger_add = window.findChild<QPushButton*>(QStringLiteral("dangerZoneAddButton"));
    auto* danger_save = window.findChild<QPushButton*>(QStringLiteral("dangerZoneSaveButton"));
    auto* danger_cancel = window.findChild<QPushButton*>(QStringLiteral("dangerZoneCancelButton"));
    auto* detection_overlay = window.findChild<logistics::control_center::DetectionOverlay*>();
    if (!check(danger_toggle != nullptr && danger_actions != nullptr && danger_settings != nullptr &&
                   danger_visibility != nullptr && danger_add != nullptr && danger_save != nullptr &&
                   danger_cancel != nullptr && detection_overlay != nullptr,
               "danger-zone controls are missing") ||
        !check(danger_actions->isHidden(), "danger-zone controls start expanded")) {
        return 1;
    }
    danger_toggle->click();
    if (!check(!danger_actions->isHidden() && !danger_settings->isHidden() && !danger_visibility->isHidden(),
               "danger-zone controls did not expand") ||
        !check(!danger_settings->isEnabled() &&
                   danger_settings->toolTip() == QStringLiteral("영상 연결 후 설정할 수 있습니다."),
               "danger-zone settings are enabled while video is offline") ||
        !check(danger_visibility->isEnabled(), "existing danger-zone visibility cannot be changed offline")) {
        return 1;
    }
    detection_overlay->beginDangerZoneEditing();
    danger_visibility->click();
    danger_visibility->click();
    if (!check(!danger_add->isHidden() && !danger_save->isHidden(), "danger-zone edit controls did not open")) {
        return 1;
    }
    danger_add->click();
    danger_save->click();
    if (!check(danger_actions->isHidden() && !danger_toggle->isChecked(),
               "danger-zone controls did not collapse after saving")) {
        return 1;
    }
    {
        QSettings saved_settings(danger_zone_config_path, QSettings::IniFormat);
        if (!check(!saved_settings.value(QStringLiteral("danger_zone/regions")).toString().isEmpty(),
                   "danger-zone regions were not persisted")) {
            return 1;
        }
    }
    danger_toggle->click();
    danger_visibility->click();
    {
        QSettings saved_settings(danger_zone_config_path, QSettings::IniFormat);
        if (!check(!saved_settings.value(QStringLiteral("danger_zone/overlay_visible"), true).toBool(),
                   "danger-zone visibility was not persisted")) {
            return 1;
        }
    }
    detection_overlay->beginDangerZoneEditing();
    danger_visibility->click();
    danger_visibility->click();
    danger_cancel->click();
    if (!check(!danger_actions->isHidden() && danger_toggle->isChecked() && !danger_settings->isHidden() &&
                   !danger_visibility->isHidden() && danger_add->isHidden() && danger_save->isHidden(),
               "danger-zone cancel did not return to the expanded default controls")) {
        return 1;
    }
    danger_toggle->click();
    if (!check(danger_actions->isHidden() && !danger_toggle->isChecked(),
               "danger-zone controls did not collapse from the default controls")) {
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
        if (qobject_cast<QStackedLayout*>(widget->layout()) != nullptr &&
            !widget->findChildren<QLabel*>(QRegularExpression(QStringLiteral("channelStatus[1-4]"))).isEmpty()) {
            video_cells.append(widget);
        }
    }
    if (!check(video_cells.size() == 1, "expected one stacked video channel cell")) {
        return 1;
    }
    auto* channel_status = window.findChild<QLabel*>(QStringLiteral("channelStatus1"));
    if (!check(channel_status != nullptr && channel_status->styleSheet().contains(QStringLiteral("border:0")),
               "video connection status label border is not transparent")) {
        return 1;
    }
    auto* video_viewport = video_cells.front()->parentWidget();
    if (!check(video_viewport != nullptr, "video grid viewport is missing")) {
        return 1;
    }

    window.resize(1280, 720);
    window.show();
    application.processEvents();
    const auto compact_danger_toggle_size = danger_toggle->size();
    if (!check(danger_toggle->text() == QStringLiteral("위험 영역 ▾"),
               "danger-zone toggle label was shortened at 1280x720")) {
        return 2;
    }
    window.resize(1600, 900);
    application.processEvents();
    const auto expanded_danger_toggle_size = danger_toggle->size();
    if (!check(expanded_danger_toggle_size.width() > compact_danger_toggle_size.width() &&
                   expanded_danger_toggle_size.height() > compact_danger_toggle_size.height(),
               "danger-zone toggle does not adapt to the video workspace size")) {
        return 2;
    }
    window.resize(1280, 720);
    application.processEvents();

    auto* severity_filter = window.findChild<QComboBox*>(QStringLiteral("logSeverityFilter"));
    auto* log_table = window.findChild<QTableView*>(QStringLiteral("operationalLogTable"));
    auto* log_flush_timer = window.findChild<QTimer*>(QStringLiteral("operationalLogFlushTimer"));
    if (!check(severity_filter != nullptr && severity_filter->view()->styleSheet().isEmpty(),
               "operational log popup overrides the shared item-view style") ||
        !check(log_table != nullptr && log_table->palette().color(QPalette::Highlight) == QColor("#264f78"),
               "operational log selection does not use the shared highlight") ||
        !check(log_table != nullptr &&
                   log_table->height() >= log_table->horizontalHeader()->height() + log_table->rowHeight(0),
               "operational log does not preserve one visible row at 1280x720") ||
        !check(log_flush_timer != nullptr && log_flush_timer->interval() == 100,
               "operational log batch timer is missing or has the wrong interval")) {
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
    log_panel->setEntryPageProvider(
        [&themed_log](qsizetype offset, qsizetype limit) { return themed_log.entries().mid(offset, limit); });
    log_panel->reloadEntries(themed_log.activeAlertCount());
    log_table->doubleClicked(log_table->model()->index(0, 3));
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
    auto* operations_dashboard = window.findChild<QWidget*>(QStringLiteral("operationsDashboard"));
    auto* app_header = window.findChild<QWidget*>(QStringLiteral("appHeader"));
    auto* central_server_status =
        app_header == nullptr ? nullptr : app_header->findChild<QLabel*>(QStringLiteral("mqttConnectionStatus"));
    const auto header_labels = app_header == nullptr ? QList<QLabel*>{} : app_header->findChildren<QLabel*>();
    const bool has_channel_badge = std::ranges::any_of(
        header_labels, [](const QLabel* label) { return label->text() == QStringLiteral("1 CHANNEL"); });
    danger_toggle->click();
    danger_settings->click();
    application.processEvents();
    if (!check(!danger_settings->isEnabled() && danger_add->isHidden() && !factory->hasFocus(),
               "offline danger-zone settings entered editing or moved focus to the factory top-view")) {
        return 2;
    }
    danger_toggle->click();
    if (!check(window.size() == QSize(1280, 720), "offscreen window did not keep 1280x720") ||
        !check(factory != nullptr && factory->isVisible(), "factoryTopView is not visible") ||
        !check(video != nullptr && video->isVisible(), "videoWorkspace is not visible") ||
        !check(process_status != nullptr && process_status->isVisible(), "processStatusSection is not visible") ||
        !check(product != nullptr && product->isVisible(), "productResultPanel is not visible") ||
        !check(log != nullptr && log->isVisible(), "operationalLogPanel is not visible") ||
        !check(process_control != nullptr && process_control->isVisible(), "processControlPanel is not visible") ||
        !check(operations_dashboard != nullptr && operations_dashboard->isVisible(),
               "operationsDashboard is not visible") ||
        !check(!has_channel_badge, "legacy 1 CHANNEL badge is still visible") ||
        !check(app_header != nullptr && !app_header->isAncestorOf(process_control),
               "processControlPanel is still contained by appHeader") ||
        !check(app_header->minimumHeight() == 46 && app_header->maximumHeight() == 58,
               "appHeader height range is not 46-58") ||
        !check(central_server_status != nullptr && central_server_status->text().contains(QStringLiteral("중앙 서버")),
               "appHeader central server status is missing") ||
        !check(central_server_status->width() == 176 && central_server_status->height() == 30,
               "central server status does not keep its fixed footprint") ||
        !check(factory->findChild<QLabel*>(QStringLiteral("factoryTopViewLiveStatus")) == nullptr,
               "factory top-view still contains the MQTT status") ||
        !check(operations_dashboard->findChild<QLabel*>(QStringLiteral("dashboardLiveStatus")) == nullptr,
               "operations dashboard still contains the duplicate MQTT status")) {
        return 2;
    }

    auto* mqtt_client = window.findChild<logistics::control_center::MqttClient*>();
    if (!check(mqtt_client != nullptr, "MqttClient is missing")) {
        return 2;
    }
    auto* command_response_timer = window.findChild<QTimer*>(QStringLiteral("commandResponseTimer"));
    if (!check(command_response_timer != nullptr, "command response timer is missing")) {
        return 2;
    }
    mqtt_client->commandPublished(1, QStringLiteral("REQ-INPUT-STOP"),
                                  logistics::contracts::mqtt::ControlCommand::kStop, QStringLiteral("pi-input-01"),
                                  QString{});
    application.processEvents();
    if (!check(command_response_timer->isActive() && command_response_timer->interval() == 17000,
               "direct input STOP did not start the target-aware 17 second watchdog")) {
        return 2;
    }
    const auto emit_input_stop_response = [&](const QString& message_id, const QString& result) {
        mqtt_client->messageReceived(
            QStringLiteral("qt/control-center/response"),
            { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
              { QStringLiteral("messageId"), message_id },
              { QStringLiteral("messageType"), QStringLiteral("COMMAND_RESPONSE") },
              { QStringLiteral("sourceId"), QStringLiteral("central-server") },
              { QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
              { QStringLiteral("data"),
                QJsonObject{ { QStringLiteral("requestId"), QStringLiteral("REQ-INPUT-STOP") },
                             { QStringLiteral("command"), QStringLiteral("STOP") },
                             { QStringLiteral("result"), result },
                             { QStringLiteral("errorCode"), QJsonValue::Null },
                             { QStringLiteral("message"), QStringLiteral("input stop progress") } } } });
        application.processEvents();
    };
    emit_input_stop_response(QStringLiteral("RESP-INPUT-STOP-PROCESSING"), QStringLiteral("PROCESSING"));
    constexpr int kProcessingAtMs = 3000;
    constexpr int kSuccessAtMs = 11000;
    if (!check(command_response_timer->isActive() && command_response_timer->interval() == 17000 &&
                   kSuccessAtMs - kProcessingAtMs < command_response_timer->interval(),
               "PROCESSING at 3 seconds did not preserve enough watchdog time for SUCCESS at 11 seconds")) {
        return 2;
    }
    emit_input_stop_response(QStringLiteral("RESP-INPUT-STOP-SUCCESS"), QStringLiteral("SUCCESS"));
    if (!check(!command_response_timer->isActive(), "SUCCESS at 11 seconds was not accepted as terminal")) {
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
                                        const QString& current_state, int timestamp_offset_ms,
                                        const QString& job_id = QStringLiteral("WORK-LAYOUT-LIVE")) {
        mqtt_client->messageReceived(
            QStringLiteral("device/%1/status").arg(source_id),
            { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
              { QStringLiteral("messageId"), message_id },
              { QStringLiteral("messageType"), QStringLiteral("DEVICE_STATUS") },
              { QStringLiteral("sourceId"), source_id },
              { QStringLiteral("timestamp"), now.addMSecs(timestamp_offset_ms).toString(Qt::ISODateWithMs) },
              { QStringLiteral("data"), QJsonObject{ { QStringLiteral("status"), QStringLiteral("ONLINE") },
                                                     { QStringLiteral("currentState"), current_state },
                                                     { QStringLiteral("jobId"), job_id } } } });
    };
    QFrame* transition_vision_card = nullptr;
    for (auto* card : window.findChildren<QFrame*>(QStringLiteral("processUnitCard"))) {
        if (card->property("controlTargetDeviceId").toString() == QStringLiteral("PI-VISION-01")) {
            transition_vision_card = card;
            break;
        }
    }
    auto* transition_vision_status =
        transition_vision_card == nullptr
            ? nullptr
            : transition_vision_card->findChild<QLabel*>(QStringLiteral("processVisualStatus"));
    if (!check(transition_vision_status != nullptr, "vision transition status is missing")) {
        return 2;
    }
    const auto check_vision_transition = [&](const QString& message_id, const QString& current_state,
                                             int timestamp_offset_ms, const QString& expected_text,
                                             const QColor& expected_color) {
        emit_device_status(message_id, QStringLiteral("PI-VISION-01"), current_state, timestamp_offset_ms);
        application.processEvents();
        return check(factory->nodeColor(QStringLiteral("vision")) == expected_color,
                     "vision transition color did not reach factoryTopView") &&
               check(transition_vision_status->text() == expected_text,
                     "vision transition text did not reach process card");
    };
    if (!check_vision_transition(QStringLiteral("LAYOUT-VISION-ONLINE"), QStringLiteral("WAITING_FOR_PRODUCT"), 20,
                                 QStringLiteral("가동 중"), QColor(QStringLiteral("#89d185"))) ||
        !check_vision_transition(QStringLiteral("LAYOUT-VISION-RUNNING"), QStringLiteral("VISION_PROCESSING"), 21,
                                 QStringLiteral("작업 중"), QColor(QStringLiteral("#75beff"))) ||
        !check_vision_transition(QStringLiteral("LAYOUT-VISION-STOPPED"), QStringLiteral("STOPPED"), 22,
                                 QStringLiteral("정지"), QColor(QStringLiteral("#cca700"))) ||
        !check_vision_transition(QStringLiteral("LAYOUT-VISION-RECOVERY"), QStringLiteral("RECOVERY"), 23,
                                 QStringLiteral("복구 중"), QColor(QStringLiteral("#c586c0")))) {
        return 2;
    }
    mqtt_client->messageReceived(
        QStringLiteral("device/PI-VISION-01/status"),
        { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
          { QStringLiteral("messageId"), QStringLiteral("LAYOUT-VISION-ERROR") },
          { QStringLiteral("messageType"), QStringLiteral("DEVICE_STATUS") },
          { QStringLiteral("sourceId"), QStringLiteral("PI-VISION-01") },
          { QStringLiteral("timestamp"), now.addMSecs(24).toString(Qt::ISODateWithMs) },
          { QStringLiteral("data"),
            QJsonObject{ { QStringLiteral("status"), QStringLiteral("ONLINE") },
                         { QStringLiteral("currentState"), QStringLiteral("ERROR") },
                         { QStringLiteral("jobId"), QStringLiteral("WORK-LAYOUT-LIVE") },
                         { QStringLiteral("errorCode"), QStringLiteral("ERR-LAYOUT-TEST") } } } });
    application.processEvents();
    if (!check(factory->nodeColor(QStringLiteral("vision")) == QColor(QStringLiteral("#f14c4c")),
               "vision error color did not reach factoryTopView") ||
        !check(transition_vision_status->text() == QStringLiteral("오류"),
               "vision error text did not reach process card")) {
        return 2;
    }
    emit_device_status(QStringLiteral("LAYOUT-VISION-RESTORED"), QStringLiteral("PI-VISION-01"),
                       QStringLiteral("VISION_PROCESSING"), 25);
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
    emit_device_status(QStringLiteral("LAYOUT-LINETRACER-LIVE"), QStringLiteral("PI-LT-01"), QStringLiteral("STOPPED"),
                       5);
    mqtt_client->messageReceived(
        QStringLiteral("device/PI-LT-01/event"),
        { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
          { QStringLiteral("messageId"), QStringLiteral("LAYOUT-LINETRACER-SENSOR-FRONT") },
          { QStringLiteral("messageType"), QStringLiteral("SENSOR_STATUS") },
          { QStringLiteral("sourceId"), QStringLiteral("PI-LT-01") },
          { QStringLiteral("timestamp"), now.addMSecs(6).toString(Qt::ISODateWithMs) },
          { QStringLiteral("data"), QJsonObject{ { QStringLiteral("sensorId"), 1 },
                                                 { QStringLiteral("sensorName"), QStringLiteral("FRONT") },
                                                 { QStringLiteral("measurementStatus"), QStringLiteral("OK") },
                                                 { QStringLiteral("detectionStatus"), QStringLiteral("DETECTED") },
                                                 { QStringLiteral("distanceCm"), 12 } } } });
    application.processEvents();

    if (!check(factory->sensorText(QStringLiteral("linetracer"), 1) == QStringLiteral("12 cm"),
               "line-tracer sensor telemetry did not refresh factoryTopView")) {
        return 2;
    }

    const auto input_before_tick = factory->boxPosition(QStringLiteral("input"));
    const auto sorting_before_tick = factory->boxPosition(QStringLiteral("sorting"));
    const auto line_arrows_before_tick = factory->lineArrowPositions();
    factory->advanceAnimationsForTest();
    if (!check(factory->boxPosition(QStringLiteral("input")) != input_before_tick,
               "input node was not moving before global emergency stop") ||
        !check(factory->boxPosition(QStringLiteral("sorting")) != sorting_before_tick,
               "sorting node was not moving before global emergency stop") ||
        !check(!line_arrows_before_tick.isEmpty() && factory->lineArrowPositions() == line_arrows_before_tick,
               "stopped line-tracer route arrows were missing or moving")) {
        return 2;
    }

    emit_device_status(QStringLiteral("LAYOUT-LINETRACER-PICKUP"), QStringLiteral("PI-LT-01"),
                       QStringLiteral("PICKUP_READY_B"), 6);
    application.processEvents();
    if (!check(factory->nodeColor(QStringLiteral("linetracer")) == QColor(QStringLiteral("#75beff")),
               "line-tracer lifecycle state did not receive the working color") ||
        !check(factory->boxPosition(QStringLiteral("linetracer")) == factory->lineTracerPickupPosition(2),
               "line-tracer pickup-ready state did not reach the live factory view")) {
        return 2;
    }
    emit_device_status(QStringLiteral("LAYOUT-LINETRACER-RETURN"), QStringLiteral("PI-LT-01"),
                       QStringLiteral("FOLLOWING_LINE"), 7);
    application.processEvents();
    const auto return_arrows_before_tick = factory->lineArrowPositions();
    factory->advanceAnimationsForTest();
    if (!check(factory->lineArrowPositions() != return_arrows_before_tick,
               "line-tracer route arrows did not resume after pickup-ready")) {
        return 2;
    }

    for (int batch = 0; log_flush_timer->isActive() && batch < 100; ++batch) {
        QMetaObject::invokeMethod(log_flush_timer, "timeout", Qt::DirectConnection);
    }
    application.processEvents();
    if (!check(!log_flush_timer->isActive(), "operational log batch queue did not drain before the timer test")) {
        return 2;
    }
    const auto log_count_before_emergency = log_table->model()->rowCount();
    mqtt_client->messageReceived(QStringLiteral("logistics/emergency-stop"),
                                 { { QStringLiteral("protocolVersion"), QStringLiteral("1.0") },
                                   { QStringLiteral("messageId"), QStringLiteral("LAYOUT-GLOBAL-EMERGENCY-STOP") },
                                   { QStringLiteral("messageType"), QStringLiteral("EMERGENCY_STOP") },
                                   { QStringLiteral("sourceId"), QStringLiteral("central-server") },
                                   { QStringLiteral("timestamp"), now.addMSecs(100).toString(Qt::ISODateWithMs) },
                                   { QStringLiteral("data"), QJsonObject{} } });
    application.processEvents();
    if (!check(log_table->model()->rowCount() == log_count_before_emergency,
               "operational log was inserted before the batch timer fired")) {
        return 2;
    }
    if (!check(QMetaObject::invokeMethod(log_flush_timer, "timeout", Qt::DirectConnection),
               "operational log batch timer could not be triggered")) {
        return 2;
    }
    application.processEvents();
    if (!check(log_table->model()->rowCount() == log_count_before_emergency + 1,
               "operational log batch was not inserted when the timer fired")) {
        return 2;
    }
    const auto stopped_input_position = factory->boxPosition(QStringLiteral("input"));
    const auto stopped_sorting_position = factory->boxPosition(QStringLiteral("sorting"));
    const auto stopped_line_position = factory->boxPosition(QStringLiteral("linetracer"));
    factory->advanceAnimationsForTest();
    factory->advanceAnimationsForTest();
    for (const auto& key : { QStringLiteral("input"), QStringLiteral("vision"), QStringLiteral("gripper"),
                             QStringLiteral("sorting"), QStringLiteral("linetracer") }) {
        if (!check(factory->nodeColor(key) == QColor(QStringLiteral("#ff3b30")),
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

    mqtt_client->commandPublished(2, QStringLiteral("REQ-SORTING-RECOVERY"),
                                  logistics::contracts::mqtt::ControlCommand::kRecovery,
                                  QStringLiteral("PI-SORTING-01"), QString{});
    emit_device_status(QStringLiteral("LAYOUT-SORTING-RECOVERED"), QStringLiteral("PI-SORTING-01"),
                       QStringLiteral("STOPPED"), 110, QString{});
    emit_device_status(QStringLiteral("LAYOUT-INPUT-RECOVERED"), QStringLiteral("PI-INPUT-01"),
                       QStringLiteral("STOPPED"), 111, QString{});
    emit_device_status(QStringLiteral("LAYOUT-VISION-RECOVERED"), QStringLiteral("PI-VISION-01"),
                       QStringLiteral("STOPPED"), 112, QString{});
    emit_device_status(QStringLiteral("LAYOUT-GRIPPER-RECOVERED"), QStringLiteral("PI-GRIPPER-01"),
                       QStringLiteral("STOPPED"), 113, QString{});
    emit_device_status(QStringLiteral("LAYOUT-LINETRACER-RECOVERED"), QStringLiteral("PI-LT-01"),
                       QStringLiteral("STOPPED"), 114, QString{});
    application.processEvents();
    auto* overall_card = window.findChild<QFrame*>(QStringLiteral("overallProcessCard"));
    bool overall_is_stopped = false;
    if (overall_card != nullptr) {
        for (const auto* label : overall_card->findChildren<QLabel*>()) {
            overall_is_stopped = overall_is_stopped || label->text() == QStringLiteral("정지");
        }
    }
    if (!check(overall_is_stopped, "recovered device states did not clear the emergency-stop screen state") ||
        !check(factory->nodeColor(QStringLiteral("sorting")) == QColor(QStringLiteral("#cca700")),
               "sorting node did not leave the emergency-stop visual state after recovery")) {
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
    const auto workspace_bottom = qMax(factory_rect.bottom(), video_rect.bottom());
    const auto process_control_gap = process_control_rect.top() - workspace_bottom - 1;
    if (!check(process_control_gap >= 5 && process_control_gap <= 7,
               "processControlPanel does not have the expected gap below the workspace") ||
        !check(process_control_rect.top() >= workspace_bottom,
               "processControlPanel is not below the video and factory workspace") ||
        !check(process_control_rect.bottom() <= process_status_rect.top(),
               "processControlPanel is not above the real-time process status")) {
        return 3;
    }
    for (const auto* cell : video_cells) {
        const QRect rect(cell->mapTo(video, QPoint{}), cell->size());
        if (!video->rect().contains(rect)) {
            std::fprintf(stderr,
                         "main_window_layout_test: live video cell geometry (%d,%d %dx%d) exceeds workspace (%dx%d)\n",
                         rect.x(), rect.y(), rect.width(), rect.height(), video->width(), video->height());
        }
        if (!check(cell->minimumSize() == QSize(480, 270), "video cell minimum is not 480x270") ||
            !check(cell->width() >= 480 && cell->height() >= 270, "video cell is smaller than 480x270") ||
            !check(video->rect().contains(rect), "video cell is clipped by videoWorkspace")) {
            return 4;
        }
    }
    auto* channel_one = window.findChild<QWidget*>(QStringLiteral("videoChannel1"));
    auto* channel_two = window.findChild<QWidget*>(QStringLiteral("videoChannel2"));
    auto* channel_three = window.findChild<QWidget*>(QStringLiteral("videoChannel3"));
    auto* channel_four = window.findChild<QWidget*>(QStringLiteral("videoChannel4"));
    if (!check(channel_one != nullptr, "video channel one is missing") ||
        !check(channel_two == nullptr && channel_three == nullptr && channel_four == nullptr,
               "unexpected additional video channels were created") ||
        !check(window.findChildren<QMediaPlayer*>().size() == 1, "expected exactly one media player")) {
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

    window.resize(1600, 900);
    application.processEvents();
    const auto expanded_factory_rect = central_rect(factory);
    const auto expanded_video_rect = central_rect(video);
    const auto expanded_process_status_rect = central_rect(process_status);
    const auto expanded_product_rect = central_rect(product);
    const auto expanded_log_rect = central_rect(log);
    const auto expanded_process_control_rect = central_rect(process_control);
    for (const auto& rect : { expanded_factory_rect, expanded_video_rect, expanded_process_status_rect,
                              expanded_product_rect, expanded_log_rect, expanded_process_control_rect }) {
        if (!check(!rect.isEmpty() && central->rect().contains(rect), "major workspace is clipped at 1600x900")) {
            return 5;
        }
    }
    if (!check(!overlaps_horizontally(expanded_factory_rect, expanded_video_rect),
               "factory and video workspaces overlap at 1600x900") ||
        !check(!overlaps_horizontally(expanded_product_rect, expanded_log_rect),
               "product and log panels overlap at 1600x900") ||
        !check(expanded_process_control_rect.top() > qMax(expanded_factory_rect.bottom(), expanded_video_rect.bottom()),
               "process controls overlap the primary workspace at 1600x900") ||
        !check(expanded_process_control_rect.bottom() <= expanded_process_status_rect.top(),
               "process controls overlap process status at 1600x900")) {
        return 5;
    }
    for (const auto* cell : video_cells) {
        const QRect rect(cell->mapTo(video, QPoint{}), cell->size());
        if (!check(video->rect().contains(rect), "video cell is clipped at 1600x900")) {
            return 5;
        }
    }
    window.resize(1280, 720);
    application.processEvents();

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
    const bool history_paging_ok = CheckHistoryPaging(application);
    if (!history_paging_ok) {
        return 9;
    }
    if (!CheckConfiguredSingleChannel(application)) {
        return 10;
    }
    return 0;
}
