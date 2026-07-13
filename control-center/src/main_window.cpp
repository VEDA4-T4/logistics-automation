#include "logistics/control_center/main_window.hpp"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QSettings>
#include <QStackedLayout>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>
#include <array>
#include <cstddef>

namespace logistics::control_center {
namespace {

constexpr int kDefaultReconnectIntervalMs = 3000;

QString controlCenterConfigPath() {
    const auto environment_path = qEnvironmentVariable("LOGISTICS_CONTROL_CENTER_CONFIG");
    if (!environment_path.isEmpty()) {
        return environment_path;
    }

    const std::array<QString, 3> candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath("config/control-center.ini"),
        QDir::current().filePath("config/control-center.ini"),
        QDir::current().filePath("control-center/config/control-center.ini"),
    };

    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return candidates.front();
}

int loadReconnectInterval() {
    QSettings settings(controlCenterConfigPath(), QSettings::IniFormat);
    bool is_valid = false;
    const auto interval = settings.value("rtsp/reconnect_interval_ms", kDefaultReconnectIntervalMs).toInt(&is_valid);
    return is_valid && interval > 0 ? interval : kDefaultReconnectIntervalMs;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Smart Logistics Control Center");
    setMinimumSize(1280, 720);
    reconnect_interval_ms_ = loadReconnectInterval();

    auto* central_widget = new QWidget(this);
    auto* video_grid = new QGridLayout(central_widget);
    video_grid->setContentsMargins(4, 4, 4, 4);
    video_grid->setSpacing(4);
    setCentralWidget(central_widget);

    stream_urls_ = {
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/0/profile2/media.smp"),
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/1/profile2/media.smp"),
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/2/profile2/media.smp"),
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/3/profile2/media.smp"),
    };

    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
        players_[channel] = new QMediaPlayer(this);
        auto* channel_panel = new QWidget(central_widget);
        auto* panel_layout = new QStackedLayout(channel_panel);
        auto* video_layer = new QWidget(channel_panel);
        auto* video_layout = new QGridLayout(video_layer);
        auto* playing_badge = new QLabel(QStringLiteral("CH %1 · 재생 중").arg(channel + 1), video_layer);
        state_overlays_[channel] = new QWidget(channel_panel);
        auto* overlay_layout = new QVBoxLayout(state_overlays_[channel]);
        auto* channel_label = new QLabel(QStringLiteral("CH %1").arg(channel + 1), state_overlays_[channel]);
        status_labels_[channel] = new QLabel(state_overlays_[channel]);
        video_widgets_[channel] = new QVideoWidget(video_layer);
        audio_outputs_[channel] = new QAudioOutput(this);
        reconnect_timers_[channel] = new QTimer(this);

        channel_panel->setStyleSheet("background-color: #111827;");
        panel_layout->setStackingMode(QStackedLayout::StackAll);
        panel_layout->setContentsMargins(0, 0, 0, 0);
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
        panel_layout->addWidget(video_layer);
        panel_layout->addWidget(state_overlays_[channel]);

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
                            setChannelState(channel, ChannelState::Connecting);
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

        setChannelState(channel, ChannelState::Connecting);
        reconnect_timers_[channel]->start();
        players_[channel]->setSource(stream_urls_[channel]);
        players_[channel]->play();

        const auto grid_index = static_cast<int>(channel);
        video_grid->addWidget(channel_panel, grid_index / 2, grid_index % 2);
    }
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
            state_overlays_[channel]->show();
            state_overlays_[channel]->raise();
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
            state_overlays_[channel]->hide();
            reconnect_timers_[channel]->stop();
            break;
        case ChannelState::Error:
            state_overlays_[channel]->show();
            state_overlays_[channel]->raise();
            status_labels_[channel]->setText(QStringLiteral("연결 오류\n%1초 간격으로 자동 재연결 중")
                                                 .arg(static_cast<double>(reconnect_interval_ms_) / 1000.0, 0, 'g', 3));
            status_labels_[channel]->setStyleSheet(
                "color: #fca5a5; background-color: transparent; font-size: 22px; font-weight: 700;");
            status_labels_[channel]->setToolTip(detail);
            state_overlays_[channel]->setToolTip(detail);
            if (!reconnect_timers_[channel]->isActive()) {
                reconnect_timers_[channel]->start();
            }
            break;
    }
}

void MainWindow::reconnectChannel(std::size_t channel) {
    if (channel_states_[channel] == ChannelState::Playing || reconnecting_[channel]) {
        return;
    }

    reconnecting_[channel] = true;
    setChannelState(channel, ChannelState::Connecting);
    players_[channel]->stop();
    players_[channel]->setSource({});

    QTimer::singleShot(0, this, [this, channel]() {
        players_[channel]->setSource(stream_urls_[channel]);
        players_[channel]->play();
        reconnecting_[channel] = false;
    });
}

}  // namespace logistics::control_center
