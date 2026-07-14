#include "logistics/control_center/main_window.hpp"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QSettings>
#include <QStackedLayout>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/control_center/mqtt_client.hpp"

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

    const auto config = loadControlCenterConfig();
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
    auto* video_grid = new QGridLayout(central_widget);
    video_grid->setContentsMargins(4, 4, 4, 4);
    video_grid->setSpacing(4);
    setCentralWidget(central_widget);

    mqtt_status_label_ = new QLabel(QStringLiteral("MQTT 연결 준비"), this);
    mqtt_status_label_->setObjectName(QStringLiteral("mqttConnectionStatus"));
    mqtt_status_label_->setMargin(4);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addPermanentWidget(mqtt_status_label_);

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
                        mqtt_status_label_->setText(QStringLiteral("MQTT 연결됨"));
                        mqtt_status_label_->setStyleSheet("color: #86efac; font-weight: 700;");
                        break;
                    case MqttClient::ConnectionState::Connecting:
                        mqtt_status_label_->setText(QStringLiteral("MQTT 연결 중"));
                        mqtt_status_label_->setStyleSheet("color: #fbbf24; font-weight: 700;");
                        break;
                    case MqttClient::ConnectionState::Reconnecting:
                        mqtt_status_label_->setText(QStringLiteral("MQTT 재연결 대기"));
                        mqtt_status_label_->setStyleSheet("color: #fb923c; font-weight: 700;");
                        break;
                    case MqttClient::ConnectionState::Error:
                        mqtt_status_label_->setText(QStringLiteral("MQTT 오류"));
                        mqtt_status_label_->setStyleSheet("color: #fca5a5; font-weight: 700;");
                        break;
                    case MqttClient::ConnectionState::Disconnected:
                        mqtt_status_label_->setText(QStringLiteral("MQTT 연결 해제"));
                        mqtt_status_label_->setStyleSheet("color: #9ca3af; font-weight: 700;");
                        break;
                }
            });
    connect(mqtt_client_, &MqttClient::messageRejected, this, [this](const QString& topic, const QString& reason) {
        statusBar()->showMessage(QStringLiteral("MQTT 메시지 거부 [%1]: %2").arg(topic, reason), 5000);
    });
    connect(mqtt_client_, &MqttClient::errorOccurred, this, [this](const QString& detail) {
        mqtt_status_label_->setToolTip(detail);
        statusBar()->showMessage(detail, 5000);
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

        channel_panel->setStyleSheet("background-color: #111827;");
        channel_stacks_[channel]->setContentsMargins(0, 0, 0, 0);
        video_layout->setContentsMargins(0, 0, 0, 0);
        video_layout->addWidget(video_widgets_[channel], 0, 0);
        video_layout->addWidget(playing_badge, 0, 0, Qt::AlignLeft | Qt::AlignTop);
        playing_badge->setMargin(8);
        playing_badge->setStyleSheet(
            "color: #d1fae5; background-color: rgba(5, 46, 22, 180); font-weight: 700;"
            "border-radius: 4px; padding: 2px 6px;");

        state_overlays_[channel]->setAttribute(Qt::WA_StyledBackground, true);
        state_overlays_[channel]->setStyleSheet("background-color: #111827;");
        overlay_layout->setContentsMargins(16, 12, 16, 12);
        channel_label->setStyleSheet("color: #9ca3af; font-weight: 700;");
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
    channel_states_[channel] = state;

    switch (state) {
        case ChannelState::Connecting:
            channel_stacks_[channel]->setCurrentWidget(state_overlays_[channel]);
            status_labels_[channel]->setText(QStringLiteral("연결 중…\n영상을 불러오는 중입니다"));
            status_labels_[channel]->setStyleSheet(
                "color: #fbbf24; background-color: transparent; font-size: 22px; font-weight: 700;");
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
                "color: #fca5a5; background-color: transparent; font-size: 22px; font-weight: 700;");
            status_labels_[channel]->setToolTip(detail);
            state_overlays_[channel]->setToolTip(detail);
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

}  // namespace logistics::control_center
