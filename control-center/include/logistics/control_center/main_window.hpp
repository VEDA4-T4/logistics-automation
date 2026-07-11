#pragma once

#include <QMainWindow>
#include <array>
#include <cstddef>

class QAudioOutput;
class QMediaPlayer;
class QVideoWidget;

namespace logistics::control_center {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    static constexpr std::size_t kChannelCount = 4;

    std::array<QMediaPlayer*, kChannelCount> players_{};
    std::array<QVideoWidget*, kChannelCount> video_widgets_{};
    std::array<QAudioOutput*, kChannelCount> audio_outputs_{};
};

}  // namespace logistics::control_center
