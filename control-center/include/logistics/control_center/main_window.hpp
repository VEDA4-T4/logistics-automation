#pragma once

#include <QList>
#include <QMainWindow>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>
#include <cstddef>
#include <memory>
#include <vector>

#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/control_center/current_product_state.hpp"
#include "logistics/control_center/operational_log_state.hpp"
#include "logistics/control_center/operations_dashboard_state.hpp"

class QJsonObject;
class QEvent;
class QLabel;
class QMediaPlayer;
class QNetworkAccessManager;
class QPushButton;
class QStackedLayout;
class QTimer;
class QWidget;

namespace logistics::control_center {

class MqttClient;
class FactoryTopViewWidget;
class OperationalLogPanel;
class OperationsDashboardPanel;
class ProductResultPanel;
class ProcessControlPanel;
class DetectionOverlay;
class OnvifRtspMetadataClient;
class RtspH264Stream;
class RtspStreamWorker;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    enum class ChannelState {
        Connecting,
        Playing,
        Error,
    };

    void updatePlaybackState(std::size_t channel);
    void setChannelState(std::size_t channel, ChannelState state, const QString& detail = {});
    void reconnectChannel(std::size_t channel);
    void sendControlCommand(logistics::contracts::mqtt::ControlCommand command, const QString& target_device_id);
    void handleMqttMessage(const QString& topic, const QJsonObject& envelope);
    void completePendingRecoveryFromDeviceState();
    void handleCommandTimeout();
    void clearPendingCommand();
    void queueOperationalLogEntry(const OperationalLogEntry& entry);
    void flushPendingOperationalLogs();
    void requestOlderOperationalLogs();
    void requestOperationalLogHistory(const QString& requested_cursor);
    void resetOperationalLogHistory();
    void appendOperationalLog(OperationalLogSeverity severity, const QString& device_id, const QString& category,
                              const QString& code, const QString& message);
    void saveDangerZoneSettings();
    void setDangerZoneEditing(bool editing, bool save_changes = false);
    void handleDangerZoneOccupancy(bool occupied, int zone_index, const QString& class_name, double confidence);
    void updateDangerZoneControls();
    void updateDangerZoneControlSizing();
    void refreshOperationsPresentation();
    void selectControlTarget(const QString& device_id, const QString& display_name);
    std::vector<QMediaPlayer*> players_{};
    std::vector<std::unique_ptr<RtspStreamWorker>> video_stream_workers_{};
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
    FactoryTopViewWidget* factory_top_view_{ nullptr };
    OperationalLogPanel* operational_log_panel_{ nullptr };
    OperationsDashboardPanel* operations_dashboard_panel_{ nullptr };
    ProductResultPanel* product_result_panel_{ nullptr };
    ProcessControlPanel* process_control_panel_{ nullptr };
    QTimer* command_response_timer_{ nullptr };
    QTimer* node_status_timer_{ nullptr };
    QTimer* operational_log_flush_timer_{ nullptr };
    QWidget* video_workspace_{ nullptr };
    QWidget* danger_zone_toolbar_{ nullptr };
    QWidget* danger_zone_channel_badge_spacer_{ nullptr };
    QWidget* danger_zone_actions_{ nullptr };
    QPushButton* danger_zone_toggle_button_{ nullptr };
    QPushButton* danger_zone_settings_button_{ nullptr };
    QPushButton* danger_zone_visibility_button_{ nullptr };
    QPushButton* danger_zone_add_button_{ nullptr };
    QPushButton* danger_zone_delete_button_{ nullptr };
    QPushButton* danger_zone_save_button_{ nullptr };
    QPushButton* danger_zone_cancel_button_{ nullptr };
    QNetworkAccessManager* history_network_manager_{ nullptr };
    QString control_target_device_id_{ "SYSTEM" };
    QString pending_target_device_id_;
    QString pending_request_id_;
    QSet<QString> individual_command_request_ids_;
    QQueue<QString> individual_command_request_order_;
    QQueue<OperationalLogEntry> pending_operational_log_entries_;
    QUrl history_base_url_;
    QString history_bearer_token_;
    QString history_next_cursor_;
    QString danger_zone_settings_path_;
    QSet<QString> history_current_page_ids_;
    logistics::contracts::mqtt::ControlCommand pending_command_{ logistics::contracts::mqtt::ControlCommand::kUnknown };
    CurrentProductState current_product_state_;
    OperationalLogState operational_log_state_;
    OperationsDashboardState operations_dashboard_state_;
    int reconnect_interval_ms_{ 3000 };
    bool rtsp_low_latency_{ true };
    int rtsp_network_timeout_ms_{ 3000 };
    qsizetype rtsp_probe_size_bytes_{ 32768 };
    qsizetype rtsp_maximum_buffer_size_bytes_{ 2 * 1024 * 1024 };
    bool onvif_metadata_enabled_{ true };
    bool onvif_log_payload_{ false };
    int metadata_stale_timeout_ms_{ 1500 };
    bool history_request_in_flight_{ false };
    bool history_page_loaded_{ false };
    bool mqtt_connected_{ false };
    bool danger_zone_controls_expanded_{ false };
    bool danger_zone_overlay_visible_{ true };
    bool danger_zone_incident_logged_{ false };
    bool danger_zone_estop_pending_{ false };
    quint64 history_request_generation_{ 0 };

    bool eventFilter(QObject* watched, QEvent* event) override;
};

}  // namespace logistics::control_center
