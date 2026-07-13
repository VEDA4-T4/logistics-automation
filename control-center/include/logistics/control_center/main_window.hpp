#pragma once

#include <QMainWindow>
#include <QString>
#include <QUrl>
#include <cstddef>
#include <vector>

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QStackedLayout;
class QTimer;
class QVideoWidget;
class QWidget;

namespace logistics::control_center {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    enum class ChannelState {
        Connecting,
        Playing,
        Error,
    };

    void updatePlaybackState(std::size_t channel);
    void setChannelState(std::size_t channel, ChannelState state, const QString& detail = {});
    void reconnectChannel(std::size_t channel);

    std::vector<QMediaPlayer*> players_{};
    std::vector<QVideoWidget*> video_widgets_{};
    std::vector<QAudioOutput*> audio_outputs_{};
    std::vector<QLabel*> status_labels_{};
    std::vector<QStackedLayout*> channel_stacks_{};
    std::vector<QWidget*> video_layers_{};
    std::vector<QWidget*> state_overlays_{};
    std::vector<QTimer*> reconnect_timers_{};
    std::vector<QUrl> stream_urls_{};
    std::vector<ChannelState> channel_states_{};
    std::vector<bool> reconnecting_{};
    QString mqtt_host_{};
    QString mqtt_client_id_{};
    std::size_t channel_count_{ 4 };
    int mqtt_port_{ 1883 };
    int reconnect_interval_ms_{ 3000 };
};

}  // namespace logistics::control_center
