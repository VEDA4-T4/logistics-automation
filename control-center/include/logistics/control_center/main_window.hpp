#pragma once

#include <QMainWindow>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>
#include <cstddef>
#include <vector>

#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/control_center/current_product_state.hpp"
#include "logistics/control_center/operational_log_state.hpp"
#include "logistics/control_center/operations_dashboard_state.hpp"

class QJsonObject;
class QLabel;
class QMediaPlayer;
class QStackedLayout;
class QTabWidget;
class QTimer;
class QWidget;

namespace logistics::control_center {

class MqttClient;
class OperationalLogPanel;
class OperationsDashboardPanel;
class ProductResultPanel;
class ProcessControlPanel;
class DetectionOverlay;
class OnvifRtspMetadataClient;
class RtspH264Stream;

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
    void sendControlCommand(logistics::contracts::mqtt::ControlCommand command, const QString& target_device_id,
                            const QString& component_id);
    void handleMqttMessage(const QString& topic, const QJsonObject& envelope);
    void completePendingRecoveryFromDeviceState();
    void handleCommandTimeout();
    void clearPendingCommand();
    void appendOperationalLog(OperationalLogSeverity severity, const QString& device_id, const QString& category,
                              const QString& code, const QString& message);
    void refreshOperationalLogPanel();
    void refreshOperationalLogBadge();

    std::vector<QMediaPlayer*> players_{};
    std::vector<RtspH264Stream*> video_streams_{};
    std::vector<QLabel*> status_labels_{};
    std::vector<QStackedLayout*> channel_stacks_{};
    std::vector<QWidget*> video_layers_{};
    std::vector<QWidget*> state_overlays_{};
    std::vector<QTimer*> reconnect_timers_{};
    std::vector<DetectionOverlay*> detection_overlays_{};
    std::vector<OnvifRtspMetadataClient*> metadata_clients_{};
    std::vector<QUrl> stream_urls_{};
    std::vector<QUrl> metadata_stream_urls_{};
    std::vector<ChannelState> channel_states_{};
    std::vector<bool> reconnecting_{};
    MqttClient* mqtt_client_{ nullptr };
    QLabel* mqtt_status_label_{ nullptr };
    QTabWidget* detail_tabs_{ nullptr };
    OperationalLogPanel* operational_log_panel_{ nullptr };
    OperationsDashboardPanel* operations_dashboard_panel_{ nullptr };
    ProductResultPanel* product_result_panel_{ nullptr };
    ProcessControlPanel* process_control_panel_{ nullptr };
    QTimer* command_response_timer_{ nullptr };
    QTimer* node_status_timer_{ nullptr };
    QString control_target_device_id_{ "SYSTEM" };
    QString pending_target_device_id_;
    QString pending_request_id_;
    QSet<QString> individual_command_request_ids_;
    QQueue<QString> individual_command_request_order_;
    logistics::contracts::mqtt::ControlCommand pending_command_{ logistics::contracts::mqtt::ControlCommand::kUnknown };
    CurrentProductState current_product_state_;
    OperationalLogState operational_log_state_;
    OperationsDashboardState operations_dashboard_state_;
    std::size_t channel_count_{ 4 };
    int reconnect_interval_ms_{ 3000 };
    bool rtsp_low_latency_{ true };
    int rtsp_network_timeout_ms_{ 3000 };
    qsizetype rtsp_probe_size_bytes_{ 32768 };
    qsizetype rtsp_maximum_buffer_size_bytes_{ 2 * 1024 * 1024 };
    bool onvif_metadata_enabled_{ true };
    bool onvif_log_payload_{ false };
    int metadata_stale_timeout_ms_{ 1500 };
};

}  // namespace logistics::control_center
