#pragma once

#include <QMainWindow>
#include <QString>
#include <QUrl>
#include <cstddef>
#include <vector>

#include "logistics/contracts/mqtt_message.hpp"

class QAudioOutput;
class QJsonObject;
class QLabel;
class QMediaPlayer;
class QStackedLayout;
class QTimer;
class QVideoWidget;
class QWidget;

namespace logistics::control_center {

class MqttClient;
class ProcessControlPanel;

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
    void sendControlCommand(logistics::contracts::mqtt::ControlCommand command);
    void handleMqttMessage(const QString& topic, const QJsonObject& envelope);
    void handleCommandTimeout();
    void clearPendingCommand();

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
    MqttClient* mqtt_client_{ nullptr };
    QLabel* mqtt_status_label_{ nullptr };
    ProcessControlPanel* process_control_panel_{ nullptr };
    QTimer* command_response_timer_{ nullptr };
    QString control_target_device_id_{ "SYSTEM" };
    QString pending_request_id_;
    logistics::contracts::mqtt::ControlCommand pending_command_{ logistics::contracts::mqtt::ControlCommand::kUnknown };
    std::size_t channel_count_{ 4 };
    int reconnect_interval_ms_{ 3000 };
};

}  // namespace logistics::control_center
