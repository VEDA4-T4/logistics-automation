#include "logistics/control_center/main_window.hpp"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QSettings>
#include <QStackedLayout>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <vector>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/control_center/command_response.hpp"
#include "logistics/control_center/mqtt_client.hpp"
#include "logistics/control_center/operational_log_panel.hpp"
#include "logistics/control_center/operations_dashboard_panel.hpp"
#include "logistics/control_center/process_control_panel.hpp"
#include "logistics/control_center/product_result_panel.hpp"

namespace logistics::control_center {
namespace {

constexpr int kDefaultReconnectIntervalMs = 3000;
constexpr int kDefaultChannelCount = 4;
constexpr int kMaximumChannelCount = 16;
constexpr int kDefaultMqttPort = 1883;

struct ControlCenterConfig {
    QString path;
    QString mqtt_host{ "127.0.0.1" };
    QString mqtt_client_id{ "control-center" };
    QString mqtt_username;
    QString mqtt_password;
    int mqtt_port{ kDefaultMqttPort };
    int mqtt_reconnect_interval_ms{ kDefaultReconnectIntervalMs };
    int mqtt_keep_alive_seconds{ 30 };
    QUrl image_base_url{ QStringLiteral("http://127.0.0.1:8080/") };
    QString control_target_device_id{ "SYSTEM" };
    QList<ProcessDefinition> process_definitions{ DefaultProcessDefinitions() };
    int channel_count{ kDefaultChannelCount };
    int reconnect_interval_ms{ kDefaultReconnectIntervalMs };
    std::vector<QUrl> stream_urls;
    QStringList warnings;
};

QString findConfigFrom(const QString& start_path) {
    QDir directory(start_path);
    constexpr int kMaximumParentSearchDepth = 6;
    const std::array<QString, 2> relative_paths = {
        QStringLiteral("config/control-centor.ini"),
        QStringLiteral("control-center/config/control-centor.ini"),
    };

    for (int depth = 0; depth <= kMaximumParentSearchDepth; ++depth) {
        for (const auto& relative_path : relative_paths) {
            const QFileInfo candidate(directory.filePath(relative_path));
            if (candidate.isFile()) {
                return candidate.canonicalFilePath();
            }
        }

        if (!directory.cdUp()) {
            break;
        }
    }

    return {};
}

QString controlCenterConfigPath() {
    const auto environment_path = qEnvironmentVariable("LOGISTICS_CONTROL_CENTER_CONFIG");
    if (!environment_path.isEmpty()) {
        return QFileInfo(environment_path).absoluteFilePath();
    }

    const std::array<QString, 2> search_roots = {
        QCoreApplication::applicationDirPath(),
        QDir::currentPath(),
    };

    for (const auto& search_root : search_roots) {
        const auto config_path = findConfigFrom(search_root);
        if (!config_path.isEmpty()) {
            return config_path;
        }
    }

    return QDir(QCoreApplication::applicationDirPath()).filePath("config/control-centor.ini");
}

bool isValidRtspUrl(const QUrl& url) {
    const auto scheme = url.scheme();
    const bool valid_scheme =
        scheme.compare("rtsp", Qt::CaseInsensitive) == 0 || scheme.compare("rtsps", Qt::CaseInsensitive) == 0;
    return url.isValid() && valid_scheme && !url.host().isEmpty();
}

bool isValidMqttHost(const QString& host) {
    const QUrl url(QStringLiteral("mqtt://") + host);
    return url.isValid() && !url.host().isEmpty() && url.port() == -1 && url.userInfo().isEmpty() &&
           url.path().isEmpty() && !url.hasQuery() && !url.hasFragment();
}

bool isValidHttpBaseUrl(const QUrl& url) {
    const auto scheme = url.scheme();
    const bool valid_scheme = scheme.compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 ||
                              scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
    return url.isValid() && valid_scheme && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() &&
           !url.hasFragment();
}

ControlCenterConfig loadControlCenterConfig() {
    ControlCenterConfig config;
    config.path = controlCenterConfigPath();

    if (!QFileInfo::exists(config.path)) {
        config.warnings.append(QStringLiteral("설정 파일을 찾을 수 없어 기본값을 사용합니다."));
    }

    QSettings settings(config.path, QSettings::IniFormat);

    const auto mqtt_host = settings.value("mqtt/host", config.mqtt_host).toString().trimmed();
    if (!isValidMqttHost(mqtt_host)) {
        config.warnings.append(QStringLiteral("mqtt/host가 잘못되어 127.0.0.1을 사용합니다."));
    } else {
        config.mqtt_host = mqtt_host;
    }

    const auto mqtt_client_id = settings.value("mqtt/client_id", config.mqtt_client_id).toString().trimmed();
    if (!logistics::contracts::mqtt::IsValidTopicLevel(mqtt_client_id.toStdString())) {
        config.warnings.append(QStringLiteral("mqtt/client_id가 비어 있거나 잘못되어 control-center를 사용합니다."));
    } else {
        config.mqtt_client_id = mqtt_client_id;
    }

    config.mqtt_username = settings.value("mqtt/username").toString();
    config.mqtt_password = settings.value("mqtt/password").toString();

    bool mqtt_port_is_valid = false;
    const auto mqtt_port = settings.value("mqtt/port", kDefaultMqttPort).toInt(&mqtt_port_is_valid);
    if (mqtt_port_is_valid && mqtt_port > 0 && mqtt_port <= 65535) {
        config.mqtt_port = mqtt_port;
    } else {
        config.warnings.append(QStringLiteral("mqtt/port가 잘못되어 1883을 사용합니다."));
    }

    bool mqtt_reconnect_interval_is_valid = false;
    const auto mqtt_reconnect_interval = settings.value("mqtt/reconnect_interval_ms", kDefaultReconnectIntervalMs)
                                             .toInt(&mqtt_reconnect_interval_is_valid);
    if (mqtt_reconnect_interval_is_valid && mqtt_reconnect_interval > 0) {
        config.mqtt_reconnect_interval_ms = mqtt_reconnect_interval;
    } else {
        config.warnings.append(QStringLiteral("mqtt/reconnect_interval_ms가 잘못되어 3000ms를 사용합니다."));
    }

    bool mqtt_keep_alive_is_valid = false;
    const auto mqtt_keep_alive = settings.value("mqtt/keep_alive_seconds", 30).toInt(&mqtt_keep_alive_is_valid);
    if (mqtt_keep_alive_is_valid && mqtt_keep_alive > 0 && mqtt_keep_alive <= 65535) {
        config.mqtt_keep_alive_seconds = mqtt_keep_alive;
    } else {
        config.warnings.append(QStringLiteral("mqtt/keep_alive_seconds가 잘못되어 30초를 사용합니다."));
    }

    const auto control_target_device_id =
        settings.value("control/target_device_id", config.control_target_device_id).toString().trimmed();
    if (!logistics::contracts::mqtt::IsValidTopicLevel(control_target_device_id.toStdString())) {
        config.warnings.append(QStringLiteral("control/target_device_id가 잘못되어 SYSTEM을 사용합니다."));
    } else {
        config.control_target_device_id = control_target_device_id;
    }

    const auto default_process_definitions = DefaultProcessDefinitions();
    for (auto& definition : config.process_definitions) {
        const auto key = QStringLiteral("dashboard/%1_device_id").arg(definition.key);
        const auto device_id = settings.value(key, definition.device_id).toString().trimmed();
        if (!logistics::contracts::mqtt::IsValidTopicLevel(device_id.toStdString())) {
            config.warnings.append(
                QStringLiteral("%1가 잘못되어 기본 장치 ID %2를 사용합니다.").arg(key, definition.device_id));
            continue;
        }
        definition.device_id = device_id;
    }
    for (qsizetype left = 0; left < config.process_definitions.size(); ++left) {
        for (qsizetype right = left + 1; right < config.process_definitions.size(); ++right) {
            if (config.process_definitions[left].device_id != config.process_definitions[right].device_id) {
                continue;
            }
            const auto reset_index =
                config.process_definitions[right].device_id != default_process_definitions[right].device_id ? right
                                                                                                            : left;
            const auto duplicate_id = config.process_definitions[reset_index].device_id;
            config.process_definitions[reset_index].device_id = default_process_definitions[reset_index].device_id;
            config.warnings.append(QStringLiteral("dashboard 장치 ID %1가 중복되어 %2 공정은 기본값 %3을 사용합니다.")
                                       .arg(duplicate_id, config.process_definitions[reset_index].display_name,
                                            config.process_definitions[reset_index].device_id));
        }
    }

    auto image_base_url = QUrl(settings.value("http/image_base_url", config.image_base_url).toString().trimmed());
    if (!isValidHttpBaseUrl(image_base_url)) {
        config.warnings.append(QStringLiteral("http/image_base_url이 잘못되어 http://127.0.0.1:8080/을 사용합니다."));
    } else {
        auto path = image_base_url.path();
        if (!path.endsWith(QLatin1Char('/'))) {
            path.append(QLatin1Char('/'));
            image_base_url.setPath(path);
        }
        config.image_base_url = image_base_url;
    }

    bool channel_count_is_valid = false;
    const auto channel_count =
        settings.value("rtsp/channel_count", kDefaultChannelCount).toInt(&channel_count_is_valid);
    if (channel_count_is_valid && channel_count > 0 && channel_count <= kMaximumChannelCount) {
        config.channel_count = channel_count;
    } else {
        config.warnings.append(QStringLiteral("rtsp/channel_count는 1~16이어야 하므로 4를 사용합니다."));
    }

    bool reconnect_interval_is_valid = false;
    const auto reconnect_interval =
        settings.value("rtsp/reconnect_interval_ms", kDefaultReconnectIntervalMs).toInt(&reconnect_interval_is_valid);
    if (reconnect_interval_is_valid && reconnect_interval > 0) {
        config.reconnect_interval_ms = reconnect_interval;
    } else {
        config.warnings.append(QStringLiteral("rtsp/reconnect_interval_ms가 잘못되어 3000ms를 사용합니다."));
    }

    QStringList invalid_channels;
    config.stream_urls.reserve(static_cast<std::size_t>(config.channel_count));
    for (int channel = 1; channel <= config.channel_count; ++channel) {
        const auto key = QStringLiteral("rtsp/channel_%1_url").arg(channel);
        const QUrl stream_url(settings.value(key).toString().trimmed());
        if (isValidRtspUrl(stream_url)) {
            config.stream_urls.push_back(stream_url);
        } else {
            config.stream_urls.emplace_back();
            invalid_channels.append(QString::number(channel));
        }
    }

    if (!invalid_channels.isEmpty()) {
        config.warnings.append(
            QStringLiteral("RTSP URL이 누락되었거나 잘못된 채널: %1").arg(invalid_channels.join(", ")));
    }

    if (settings.status() == QSettings::AccessError) {
        config.warnings.append(QStringLiteral("설정 파일을 읽을 수 없습니다."));
    } else if (settings.status() == QSettings::FormatError) {
        config.warnings.append(QStringLiteral("설정 파일 형식이 올바르지 않습니다."));
    }

    return config;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Smart Logistics Control Center");
    setMinimumSize(1280, 720);
    setStyleSheet(
        "QMainWindow{background:#1f1f1f;}"
        "QStatusBar{background:#181818;color:#cccccc;border-top:1px solid #2b2b2b;}"
        "QToolTip{background:#252526;color:#f0f0f0;border:1px solid #454545;padding:5px;}");

    const auto config = loadControlCenterConfig();
    control_target_device_id_ = config.control_target_device_id;
    operations_dashboard_state_.configureProcesses(config.process_definitions);
    channel_count_ = static_cast<std::size_t>(config.channel_count);
    reconnect_interval_ms_ = config.reconnect_interval_ms;
    stream_urls_ = config.stream_urls;
    players_.resize(channel_count_);
    video_widgets_.resize(channel_count_);
    audio_outputs_.resize(channel_count_);
    status_labels_.resize(channel_count_);
    channel_stacks_.resize(channel_count_);
    video_layers_.resize(channel_count_);
    state_overlays_.resize(channel_count_);
    reconnect_timers_.resize(channel_count_);
    channel_states_.assign(channel_count_, ChannelState::Connecting);
    reconnecting_.assign(channel_count_, false);

    auto* central_widget = new QWidget(this);
    central_widget->setObjectName(QStringLiteral("centralSurface"));
    central_widget->setStyleSheet("#centralSurface{background:#1f1f1f;}");
    auto* root_layout = new QVBoxLayout(central_widget);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* app_header = new QFrame(central_widget);
    app_header->setObjectName(QStringLiteral("appHeader"));
    app_header->setStyleSheet("#appHeader{background:#181818;border-bottom:1px solid #2b2b2b;}");
    app_header->setMinimumHeight(54);
    app_header->setMaximumHeight(54);
    auto* app_header_layout = new QHBoxLayout(app_header);
    app_header_layout->setContentsMargins(16, 7, 16, 7);
    auto* app_title_layout = new QVBoxLayout();
    app_title_layout->setContentsMargins(0, 0, 0, 0);
    app_title_layout->setSpacing(1);
    auto* app_eyebrow = new QLabel(QStringLiteral("SMART LOGISTICS"), app_header);
    app_eyebrow->setStyleSheet("color:#4daafc;font-size:9px;font-weight:700;letter-spacing:1px;");
    auto* app_title = new QLabel(QStringLiteral("물류 자동화 통합 관제센터"), app_header);
    app_title->setStyleSheet("color:#f0f0f0;font-size:18px;font-weight:700;");
    app_title_layout->addWidget(app_eyebrow);
    app_title_layout->addWidget(app_title);
    auto* channel_badge = new QLabel(QStringLiteral("%1 CHANNELS").arg(channel_count_), app_header);
    channel_badge->setAlignment(Qt::AlignCenter);
    channel_badge->setStyleSheet(
        "background:#252526;color:#cccccc;border:1px solid #3c3c3c;border-radius:4px;"
        "font-size:10px;font-weight:700;padding:5px 10px;");
    app_header_layout->addLayout(app_title_layout);
    app_header_layout->addStretch();
    app_header_layout->addWidget(channel_badge);
    root_layout->addWidget(app_header);

    operations_dashboard_panel_ = new OperationsDashboardPanel(central_widget);
    operations_dashboard_panel_->setState(operations_dashboard_state_);
    root_layout->addWidget(operations_dashboard_panel_);

    auto* content = new QWidget(central_widget);
    auto* content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(10, 10, 10, 10);
    content_layout->setSpacing(10);

    auto* video_container = new QWidget(content);
    auto* video_grid = new QGridLayout(video_container);
    video_grid->setContentsMargins(0, 0, 0, 0);
    video_grid->setSpacing(8);
    content_layout->addWidget(video_container, 1);

    auto* side_panel = new QWidget(content);
    side_panel->setObjectName(QStringLiteral("sidePanel"));
    side_panel->setMinimumWidth(390);
    side_panel->setMaximumWidth(390);
    side_panel->setStyleSheet("#sidePanel{background:transparent;}");
    auto* side_layout = new QVBoxLayout(side_panel);
    side_layout->setContentsMargins(0, 0, 0, 0);
    side_layout->setSpacing(10);
    detail_tabs_ = new QTabWidget(side_panel);
    detail_tabs_->setObjectName(QStringLiteral("detailTabs"));
    detail_tabs_->setDocumentMode(true);
    detail_tabs_->setStyleSheet(
        "QTabWidget::pane{border:1px solid #2b2b2b;background:#181818;}"
        "QTabBar::tab{background:#252526;color:#9d9d9d;border:1px solid #2b2b2b;padding:7px 14px;}"
        "QTabBar::tab:selected{background:#181818;color:#f0f0f0;border-bottom:2px solid #4daafc;}");
    product_result_panel_ = new ProductResultPanel(config.image_base_url, detail_tabs_);
    operational_log_panel_ = new OperationalLogPanel(detail_tabs_);
    detail_tabs_->addTab(product_result_panel_, QStringLiteral("현재 상품"));
    detail_tabs_->addTab(operational_log_panel_, QStringLiteral("운영 로그"));
    process_control_panel_ = new ProcessControlPanel(side_panel);
    side_layout->addWidget(detail_tabs_, 1);
    side_layout->addWidget(process_control_panel_, 0);
    content_layout->addWidget(side_panel);
    root_layout->addWidget(content, 1);
    setCentralWidget(central_widget);

    mqtt_status_label_ = new QLabel(QStringLiteral("MQTT 연결 준비"), this);
    mqtt_status_label_->setObjectName(QStringLiteral("mqttConnectionStatus"));
    mqtt_status_label_->setMargin(4);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addPermanentWidget(mqtt_status_label_);

    command_response_timer_ = new QTimer(this);
    command_response_timer_->setSingleShot(true);
    connect(command_response_timer_, &QTimer::timeout, this, &MainWindow::handleCommandTimeout);
    connect(process_control_panel_, &ProcessControlPanel::commandRequested, this, &MainWindow::sendControlCommand);
    operational_log_panel_->setAcknowledgeHandler([this](const QString& id) {
        if (operational_log_state_.acknowledge(id)) {
            refreshOperationalLogPanel();
        }
    });

    mqtt_client_ = new MqttClient({ .host = config.mqtt_host,
                                    .client_id = config.mqtt_client_id,
                                    .username = config.mqtt_username,
                                    .password = config.mqtt_password,
                                    .port = config.mqtt_port,
                                    .reconnect_interval_ms = config.mqtt_reconnect_interval_ms,
                                    .keep_alive_seconds = config.mqtt_keep_alive_seconds },
                                  this);
    connect(mqtt_client_, &MqttClient::connectionStateChanged, this,
            [this](MqttClient::ConnectionState state, const QString& detail) {
                mqtt_status_label_->setToolTip(detail);
                switch (state) {
                    case MqttClient::ConnectionState::Connected:
                        process_control_panel_->setMqttConnected(true);
                        operations_dashboard_panel_->setMqttConnected(true);
                        mqtt_status_label_->setText(QStringLiteral("MQTT 연결됨"));
                        mqtt_status_label_->setStyleSheet("color:#89d185;font-weight:700;");
                        appendOperationalLog(OperationalLogSeverity::Info, QStringLiteral("central-server"),
                                             QStringLiteral("통신"), QStringLiteral("MQTT_CONNECTED"), detail);
                        break;
                    case MqttClient::ConnectionState::Connecting:
                        process_control_panel_->setMqttConnected(false);
                        operations_dashboard_panel_->setMqttConnected(false);
                        clearPendingCommand();
                        mqtt_status_label_->setText(QStringLiteral("MQTT 연결 중"));
                        mqtt_status_label_->setStyleSheet("color:#cca700;font-weight:700;");
                        break;
                    case MqttClient::ConnectionState::Reconnecting:
                        process_control_panel_->setMqttConnected(false);
                        operations_dashboard_panel_->setMqttConnected(false);
                        clearPendingCommand();
                        mqtt_status_label_->setText(QStringLiteral("MQTT 재연결 대기"));
                        mqtt_status_label_->setStyleSheet("color:#ce9178;font-weight:700;");
                        appendOperationalLog(OperationalLogSeverity::Warning, QStringLiteral("central-server"),
                                             QStringLiteral("통신 장애"), QStringLiteral("MQTT_RECONNECTING"), detail);
                        break;
                    case MqttClient::ConnectionState::Error:
                        process_control_panel_->setMqttConnected(false);
                        operations_dashboard_panel_->setMqttConnected(false);
                        clearPendingCommand();
                        mqtt_status_label_->setText(QStringLiteral("MQTT 오류"));
                        mqtt_status_label_->setStyleSheet("color:#f14c4c;font-weight:700;");
                        break;
                    case MqttClient::ConnectionState::Disconnected:
                        process_control_panel_->setMqttConnected(false);
                        operations_dashboard_panel_->setMqttConnected(false);
                        clearPendingCommand();
                        mqtt_status_label_->setText(QStringLiteral("MQTT 연결 해제"));
                        mqtt_status_label_->setStyleSheet("color:#9d9d9d;font-weight:700;");
                        appendOperationalLog(OperationalLogSeverity::Warning, QStringLiteral("central-server"),
                                             QStringLiteral("통신 장애"), QStringLiteral("MQTT_DISCONNECTED"), detail);
                        break;
                }
            });
    connect(mqtt_client_, &MqttClient::commandPublished, this,
            [this](qint32, const QString& request_id, logistics::contracts::mqtt::ControlCommand command) {
                pending_request_id_ = request_id;
                pending_command_ = command;
                process_control_panel_->setCommandPending(command);

                const auto timeout = command == logistics::contracts::mqtt::ControlCommand::kEmergencyStop
                                         ? logistics::contracts::mqtt::kEmergencyStopConfirmationTimeout
                                         : logistics::contracts::mqtt::kMqttResponseTimeout;
                command_response_timer_->start(
                    static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
            });
    connect(mqtt_client_, &MqttClient::messageReceived, this, &MainWindow::handleMqttMessage);
    connect(mqtt_client_, &MqttClient::messageRejected, this, [this](const QString& topic, const QString& reason) {
        statusBar()->showMessage(QStringLiteral("MQTT 메시지 거부 [%1]: %2").arg(topic, reason), 5000);
        appendOperationalLog(OperationalLogSeverity::Warning, QStringLiteral("central-server"),
                             QStringLiteral("메시지 검증"), QStringLiteral("MQTT_MESSAGE_REJECTED"),
                             QStringLiteral("%1 · %2").arg(topic, reason));
    });
    connect(mqtt_client_, &MqttClient::errorOccurred, this, [this](const QString& detail) {
        mqtt_status_label_->setToolTip(detail);
        statusBar()->showMessage(detail, 5000);
        appendOperationalLog(OperationalLogSeverity::Error, QStringLiteral("central-server"),
                             QStringLiteral("통신 장애"), QStringLiteral("MQTT_ERROR"), detail);
    });

    const auto grid_column_count = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(channel_count_))));

    for (std::size_t channel = 0; channel < channel_count_; ++channel) {
        players_[channel] = new QMediaPlayer(this);
        auto* channel_panel = new QWidget(central_widget);
        channel_stacks_[channel] = new QStackedLayout(channel_panel);
        video_layers_[channel] = new QWidget(channel_panel);
        auto* video_layout = new QGridLayout(video_layers_[channel]);
        auto* playing_badge = new QLabel(QStringLiteral("CH %1 · 재생 중").arg(channel + 1), video_layers_[channel]);
        state_overlays_[channel] = new QWidget(channel_panel);
        auto* overlay_layout = new QVBoxLayout(state_overlays_[channel]);
        auto* channel_label = new QLabel(QStringLiteral("CH %1").arg(channel + 1), state_overlays_[channel]);
        status_labels_[channel] = new QLabel(state_overlays_[channel]);
        video_widgets_[channel] = new QVideoWidget(video_layers_[channel]);
        audio_outputs_[channel] = new QAudioOutput(this);
        reconnect_timers_[channel] = new QTimer(this);

        channel_panel->setStyleSheet("background-color:#181818;border:1px solid #2b2b2b;border-radius:6px;");
        channel_stacks_[channel]->setContentsMargins(0, 0, 0, 0);
        video_layout->setContentsMargins(0, 0, 0, 0);
        video_layout->addWidget(video_widgets_[channel], 0, 0);
        video_layout->addWidget(playing_badge, 0, 0, Qt::AlignLeft | Qt::AlignTop);
        playing_badge->setMargin(8);
        playing_badge->setStyleSheet(
            "color:#b5cea8;background-color:rgba(24,24,24,220);border:1px solid #3c3c3c;"
            "font-weight:700;border-radius:4px;padding:4px 8px;");

        state_overlays_[channel]->setAttribute(Qt::WA_StyledBackground, true);
        state_overlays_[channel]->setStyleSheet("background-color:#181818;border-radius:5px;");
        overlay_layout->setContentsMargins(16, 12, 16, 12);
        channel_label->setStyleSheet("color:#cccccc;font-size:12px;font-weight:700;");
        status_labels_[channel]->setObjectName(QStringLiteral("channelStatus%1").arg(channel + 1));
        status_labels_[channel]->setAlignment(Qt::AlignCenter);
        status_labels_[channel]->setWordWrap(true);
        overlay_layout->addWidget(channel_label, 0, Qt::AlignLeft | Qt::AlignTop);
        overlay_layout->addStretch();
        overlay_layout->addWidget(status_labels_[channel], 0, Qt::AlignCenter);
        overlay_layout->addStretch();
        channel_stacks_[channel]->addWidget(video_layers_[channel]);
        channel_stacks_[channel]->addWidget(state_overlays_[channel]);

        video_widgets_[channel]->setMinimumSize(320, 180);
        players_[channel]->setVideoOutput(video_widgets_[channel]);
        players_[channel]->setAudioOutput(audio_outputs_[channel]);
        audio_outputs_[channel]->setVolume(0.0F);
        reconnect_timers_[channel]->setInterval(reconnect_interval_ms_);

        connect(reconnect_timers_[channel], &QTimer::timeout, this, [this, channel]() { reconnectChannel(channel); });
        connect(players_[channel], &QMediaPlayer::errorOccurred, this,
                [this, channel](QMediaPlayer::Error error, const QString& description) {
                    if (error != QMediaPlayer::NoError) {
                        const auto detail = description.isEmpty() ? QStringLiteral("영상 연결 오류") : description;
                        setChannelState(channel, ChannelState::Error, detail);
                    }
                });
        connect(players_[channel], &QMediaPlayer::mediaStatusChanged, this,
                [this, channel](QMediaPlayer::MediaStatus status) {
                    switch (status) {
                        case QMediaPlayer::LoadingMedia:
                            if (channel_states_[channel] != ChannelState::Error) {
                                setChannelState(channel, ChannelState::Connecting);
                            }
                            break;
                        case QMediaPlayer::LoadedMedia:
                        case QMediaPlayer::BufferingMedia:
                        case QMediaPlayer::BufferedMedia:
                            updatePlaybackState(channel);
                            break;
                        case QMediaPlayer::StalledMedia:
                            setChannelState(channel, ChannelState::Error, QStringLiteral("영상 연결이 끊겼습니다"));
                            break;
                        case QMediaPlayer::EndOfMedia:
                            setChannelState(channel, ChannelState::Error,
                                            QStringLiteral("영상 스트림이 종료되었습니다"));
                            break;
                        case QMediaPlayer::InvalidMedia:
                            setChannelState(channel, ChannelState::Error, players_[channel]->errorString());
                            break;
                        case QMediaPlayer::NoMedia:
                            if (!reconnecting_[channel]) {
                                setChannelState(channel, ChannelState::Error, QStringLiteral("영상 소스가 없습니다"));
                            }
                            break;
                    }
                });
        connect(players_[channel], &QMediaPlayer::playbackStateChanged, this,
                [this, channel](QMediaPlayer::PlaybackState state) {
                    if (reconnecting_[channel]) {
                        return;
                    }
                    if (state == QMediaPlayer::PlayingState) {
                        updatePlaybackState(channel);
                    } else if (channel_states_[channel] == ChannelState::Playing) {
                        setChannelState(channel, ChannelState::Error, QStringLiteral("영상 재생이 중단되었습니다"));
                    }
                });
        connect(players_[channel], &QMediaPlayer::hasVideoChanged, this, [this, channel](bool has_video) {
            if (has_video) {
                updatePlaybackState(channel);
            } else if (!reconnecting_[channel] && channel_states_[channel] == ChannelState::Playing) {
                setChannelState(channel, ChannelState::Error, QStringLiteral("영상 신호가 끊겼습니다"));
            }
        });

        if (stream_urls_[channel].isEmpty()) {
            setChannelState(channel, ChannelState::Error, QStringLiteral("RTSP URL 설정을 확인하세요"));
        } else {
            setChannelState(channel, ChannelState::Connecting);
            reconnect_timers_[channel]->start();
            players_[channel]->setSource(stream_urls_[channel]);
            players_[channel]->play();
        }

        const auto grid_index = static_cast<int>(channel);
        video_grid->addWidget(channel_panel, grid_index / grid_column_count, grid_index % grid_column_count);
    }

    if (!config.warnings.isEmpty()) {
        for (const auto& warning : config.warnings) {
            appendOperationalLog(OperationalLogSeverity::Warning, QStringLiteral("control-center"),
                                 QStringLiteral("설정"), QStringLiteral("CONFIG_WARNING"), warning);
        }
        const auto warning_message =
            QStringLiteral("설정 파일: %1\n\n%2")
                .arg(QDir::toNativeSeparators(config.path), config.warnings.join(QLatin1Char('\n')));
        QTimer::singleShot(0, this, [this, warning_message]() {
            QMessageBox::warning(this, QStringLiteral("설정 확인"), warning_message);
        });
    }

    mqtt_client_->start();
}

void MainWindow::updatePlaybackState(std::size_t channel) {
    const auto media_status = players_[channel]->mediaStatus();
    const bool media_is_ready = media_status == QMediaPlayer::LoadedMedia ||
                                media_status == QMediaPlayer::BufferingMedia ||
                                media_status == QMediaPlayer::BufferedMedia;
    if (players_[channel]->playbackState() == QMediaPlayer::PlayingState && media_is_ready &&
        players_[channel]->hasVideo()) {
        setChannelState(channel, ChannelState::Playing);
    }
}

void MainWindow::setChannelState(std::size_t channel, ChannelState state, const QString& detail) {
    const auto previous_state = channel_states_[channel];
    channel_states_[channel] = state;

    switch (state) {
        case ChannelState::Connecting:
            channel_stacks_[channel]->setCurrentWidget(state_overlays_[channel]);
            status_labels_[channel]->setText(QStringLiteral("연결 중…\n영상을 불러오는 중입니다"));
            status_labels_[channel]->setStyleSheet(
                "color:#cca700;background-color:transparent;font-size:22px;font-weight:700;");
            status_labels_[channel]->setToolTip({});
            state_overlays_[channel]->setToolTip({});
            break;
        case ChannelState::Playing:
            status_labels_[channel]->setText(QStringLiteral("재생 중"));
            status_labels_[channel]->setToolTip({});
            state_overlays_[channel]->setToolTip({});
            channel_stacks_[channel]->setCurrentWidget(video_layers_[channel]);
            reconnect_timers_[channel]->stop();
            break;
        case ChannelState::Error:
            channel_stacks_[channel]->setCurrentWidget(state_overlays_[channel]);
            if (stream_urls_[channel].isEmpty()) {
                status_labels_[channel]->setText(QStringLiteral("설정 오류\nRTSP URL을 확인하세요"));
                reconnect_timers_[channel]->stop();
            } else {
                status_labels_[channel]->setText(
                    QStringLiteral("연결 안 됨\n%1초 간격으로 자동 재연결 중")
                        .arg(static_cast<double>(reconnect_interval_ms_) / 1000.0, 0, 'g', 3));
                if (!reconnect_timers_[channel]->isActive()) {
                    reconnect_timers_[channel]->start();
                }
            }
            status_labels_[channel]->setStyleSheet(
                "color:#f14c4c;background-color:transparent;font-size:22px;font-weight:700;");
            status_labels_[channel]->setToolTip(detail);
            state_overlays_[channel]->setToolTip(detail);
            if (previous_state != ChannelState::Error) {
                appendOperationalLog(OperationalLogSeverity::Error, QStringLiteral("RTSP-CH-%1").arg(channel + 1),
                                     QStringLiteral("영상 통신"), QStringLiteral("RTSP_ERROR"),
                                     detail.isEmpty() ? QStringLiteral("영상 연결 또는 재생에 실패했습니다.") : detail);
            }
            break;
    }
}

void MainWindow::reconnectChannel(std::size_t channel) {
    if (stream_urls_[channel].isEmpty() || channel_states_[channel] == ChannelState::Playing ||
        reconnecting_[channel]) {
        return;
    }

    reconnecting_[channel] = true;
    setChannelState(channel, ChannelState::Error, QStringLiteral("연결 시간 초과 또는 연결 실패"));
    players_[channel]->stop();
    players_[channel]->setSource({});

    QTimer::singleShot(0, this, [this, channel]() {
        players_[channel]->setSource(stream_urls_[channel]);
        players_[channel]->play();
        reconnecting_[channel] = false;
    });
}

void MainWindow::sendControlCommand(logistics::contracts::mqtt::ControlCommand command) {
    const auto message_id = mqtt_client_->publishCommand(command, control_target_device_id_);
    if (message_id < 0) {
        process_control_panel_->setCommandFinished(command, logistics::contracts::mqtt::CommandResult::kFailed,
                                                   QStringLiteral("MQTT 명령 발행 실패"));
    }
}

void MainWindow::handleMqttMessage(const QString& topic, const QJsonObject& envelope) {
    const auto parsed_topic = logistics::contracts::mqtt::ParseTopic(topic.toStdString());
    const auto log_update = operational_log_state_.applyEnvelope(topic, envelope);
    if (log_update.applied) {
        refreshOperationalLogPanel();
    } else if (log_update.handled && !log_update.error.isEmpty()) {
        statusBar()->showMessage(log_update.error, 4000);
    }
    const auto dashboard_update = operations_dashboard_state_.applyEnvelope(envelope);
    if (dashboard_update.applied) {
        operations_dashboard_panel_->setState(operations_dashboard_state_);
    } else if (dashboard_update.handled && !dashboard_update.error.isEmpty()) {
        statusBar()->showMessage(dashboard_update.error, 4000);
    }

    if (parsed_topic.kind == logistics::contracts::mqtt::TopicKind::kQtEvent ||
        parsed_topic.kind == logistics::contracts::mqtt::TopicKind::kQtStatus) {
        const auto product_update = current_product_state_.applyEnvelope(envelope);
        if (product_update.handled) {
            if (product_update.applied) {
                product_result_panel_->setCurrentProduct(current_product_state_.product());
            } else if (!product_update.error.isEmpty()) {
                statusBar()->showMessage(product_update.error, 4000);
            }
            return;
        }
    }

    if (parsed_topic.kind != logistics::contracts::mqtt::TopicKind::kQtResponse) {
        return;
    }

    const auto response = ParseCommandResponse(envelope);
    if (!response.is_valid) {
        statusBar()->showMessage(QStringLiteral("명령 응답 거부: %1").arg(response.error), 5000);
        return;
    }
    if (pending_request_id_.isEmpty() || response.request_id != pending_request_id_) {
        statusBar()->showMessage(QStringLiteral("현재 요청과 일치하지 않는 명령 응답을 무시했습니다."), 3000);
        return;
    }
    if (response.command != pending_command_) {
        statusBar()->showMessage(QStringLiteral("요청 명령과 응답 명령이 일치하지 않습니다."), 5000);
        return;
    }

    QString detail = response.message;
    if (!response.error_code.isEmpty()) {
        detail = detail.isEmpty() ? response.error_code : QStringLiteral("%1 · %2").arg(response.error_code, detail);
    }

    if (logistics::contracts::mqtt::IsTerminal(response.result)) {
        command_response_timer_->stop();
        process_control_panel_->setCommandFinished(response.command, response.result, detail);
        clearPendingCommand();
        return;
    }

    process_control_panel_->setCommandProgress(response.command, response.result, detail);
    const auto timeout = response.command == logistics::contracts::mqtt::ControlCommand::kEmergencyStop
                             ? logistics::contracts::mqtt::kEmergencyStopConfirmationTimeout
                             : logistics::contracts::mqtt::kMqttResponseTimeout;
    command_response_timer_->start(
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
}

void MainWindow::handleCommandTimeout() {
    if (pending_command_ == logistics::contracts::mqtt::ControlCommand::kUnknown) {
        return;
    }
    process_control_panel_->setCommandFinished(pending_command_, logistics::contracts::mqtt::CommandResult::kTimeout);
    appendOperationalLog(OperationalLogSeverity::Error, control_target_device_id_, QStringLiteral("관제 명령"),
                         QStringLiteral("COMMAND_TIMEOUT"), QStringLiteral("관제 명령 응답 시간이 초과되었습니다."));
    clearPendingCommand();
}

void MainWindow::clearPendingCommand() {
    command_response_timer_->stop();
    pending_request_id_.clear();
    pending_command_ = logistics::contracts::mqtt::ControlCommand::kUnknown;
}

void MainWindow::appendOperationalLog(OperationalLogSeverity severity, const QString& device_id,
                                      const QString& category, const QString& code, const QString& message) {
    operational_log_state_.appendLocal(severity, device_id, category, code, message);
    refreshOperationalLogPanel();
}

void MainWindow::refreshOperationalLogPanel() {
    if (operational_log_panel_ == nullptr || detail_tabs_ == nullptr) {
        return;
    }
    operational_log_panel_->setState(operational_log_state_);
    const auto alert_count = operational_log_state_.activeAlertCount();
    detail_tabs_->setTabText(
        1, alert_count > 0 ? QStringLiteral("운영 로그 (%1)").arg(alert_count) : QStringLiteral("운영 로그"));
}

}  // namespace logistics::control_center
