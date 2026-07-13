#pragma once

#include <QMainWindow>
#include <QString>
#include <QUrl>
#include <array>
#include <cstddef>

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QTimer;
class QVideoWidget;
class QWidget;

namespace logistics::control_center {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    static constexpr std::size_t kChannelCount = 4;

    enum class ChannelState {
        Connecting,
        Playing,
        Error,
    };

    void updatePlaybackState(std::size_t channel);
    void setChannelState(std::size_t channel, ChannelState state, const QString& detail = {});
    void reconnectChannel(std::size_t channel);

    std::array<QMediaPlayer*, kChannelCount> players_{};
    std::array<QVideoWidget*, kChannelCount> video_widgets_{};
    std::array<QAudioOutput*, kChannelCount> audio_outputs_{};
    std::array<QLabel*, kChannelCount> status_labels_{};
    std::array<QWidget*, kChannelCount> state_overlays_{};
    std::array<QTimer*, kChannelCount> reconnect_timers_{};
    std::array<QUrl, kChannelCount> stream_urls_{};
    std::array<ChannelState, kChannelCount> channel_states_{};
    std::array<bool, kChannelCount> reconnecting_{};
    int reconnect_interval_ms_{ 3000 };
};

}  // namespace logistics::control_center
