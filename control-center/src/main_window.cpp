#include "logistics/control_center/main_window.hpp"

#include <QAudioOutput>
#include <QGridLayout>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoWidget>
#include <QWidget>

#include <array>
#include <cstddef>

namespace logistics::control_center {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Smart Logistics Control Center");
    setMinimumSize(1280, 720);

    auto* central_widget = new QWidget(this);
    auto* video_grid = new QGridLayout(central_widget);
    video_grid->setContentsMargins(4, 4, 4, 4);
    video_grid->setSpacing(4);
    setCentralWidget(central_widget);

    const std::array<QUrl, kChannelCount> stream_urls = {
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/0/profile2/media.smp"),
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/1/profile2/media.smp"),
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/2/profile2/media.smp"),
        QUrl("rtsp://admin:5hanwha%21@veda4-t4.iptime.org:8554/3/profile2/media.smp"),
    };

    for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
        players_[channel] = new QMediaPlayer(this);
        video_widgets_[channel] = new QVideoWidget(central_widget);
        audio_outputs_[channel] = new QAudioOutput(this);

        video_widgets_[channel]->setMinimumSize(320, 180);
        players_[channel]->setVideoOutput(video_widgets_[channel]);
        players_[channel]->setAudioOutput(audio_outputs_[channel]);
        audio_outputs_[channel]->setVolume(0.0F);

        players_[channel]->setSource(stream_urls[channel]);
        players_[channel]->play();

        const auto grid_index = static_cast<int>(channel);
        video_grid->addWidget(video_widgets_[channel], grid_index / 2, grid_index % 2);
    }
}

}  // namespace logistics::control_center
