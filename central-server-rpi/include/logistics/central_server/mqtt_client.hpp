#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/central_server/mqtt_transport.hpp"

namespace logistics::contracts::mqtt {

struct MqttMessage;
enum class Qos : std::uint8_t;
enum class ConnectionState : std::uint8_t;

}  // namespace logistics::contracts::mqtt

namespace logistics::central_server {

enum class MqttLogLevel : std::uint8_t {
    kInfo,
    kWarning,
    kError,
};

class MqttClient final {
public:
    using Logger = std::function<void(MqttLogLevel level, std::string_view message)>;
    using MessageHandler =
        std::function<void(std::string_view topic, std::string_view payload, int qos, bool retained)>;
    using PublishAcknowledgedHandler = std::function<void(int packet_id)>;
    using DisconnectedHandler = std::function<void()>;

    MqttClient(MqttConfig config, std::unique_ptr<MqttTransport> transport);
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    MqttClient(MqttClient&&) = delete;
    MqttClient& operator=(MqttClient&&) = delete;

    void SetLogger(Logger logger);
    void SetMessageHandler(MessageHandler handler);
    void SetDisconnectedHandler(DisconnectedHandler handler);
    [[nodiscard]] bool SetWill(MqttWill will);

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] bool IsReady() const noexcept;

    [[nodiscard]] bool Publish(std::string_view topic, std::string_view payload, int qos = 1, bool retain = false);
    [[nodiscard]] std::optional<int> PublishWithReceipt(std::string_view topic, std::string_view payload, int qos = 1,
                                                        bool retain = false);
    [[nodiscard]] bool PublishTransientMessage(std::string_view topic, const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] bool PublishMessage(std::string_view topic, const contracts::mqtt::MqttMessage& message,
                                      contracts::mqtt::Qos qos, bool retain = false);
    [[nodiscard]] std::optional<int> PublishMessageWithReceipt(std::string_view topic,
                                                               const contracts::mqtt::MqttMessage& message,
                                                               contracts::mqtt::Qos qos, bool retain = false);
    [[nodiscard]] bool PublishMessageAcknowledged(std::string_view topic, const contracts::mqtt::MqttMessage& message,
                                                  contracts::mqtt::Qos qos, PublishAcknowledgedHandler acknowledged,
                                                  bool retain = false);

private:
    void HandleConnected(int reason_code, std::string_view reason);
    void HandleDisconnected(int reason_code, std::string_view reason);
    void HandleMessage(std::string_view topic, std::string_view payload, int qos, bool retained);
    void HandlePublishAcknowledged(int packet_id);
    void HandleSubscribeAcknowledged(int packet_id, const std::vector<int>& granted_qos);
    void HandleTransportLog(MqttTransportLogLevel level, std::string_view message);
    void SubscribeRequiredTopics();
    [[nodiscard]] std::optional<int> PublishWithReceiptUnchecked(std::string_view topic, std::string_view payload,
                                                                 int qos, bool retain);
    [[nodiscard]] contracts::mqtt::MqttMessage MakeServerStatusMessage(contracts::mqtt::ConnectionState status);
    [[nodiscard]] bool PublishServerStatus(contracts::mqtt::ConnectionState status);
    [[nodiscard]] bool PublishHeartbeat();
    void StartHeartbeatLoop();
    void Log(MqttLogLevel level, std::string_view message) const;

    MqttConfig config_;
    std::unique_ptr<MqttTransport> transport_;
    std::atomic_bool connected_{ false };
    std::atomic_bool ready_{ false };
    std::atomic_bool subscription_failed_{ false };
    std::atomic_bool running_{ false };
    std::atomic_bool stopping_{ false };
    mutable std::mutex callback_mutex_;
    std::mutex publish_registration_mutex_;
    Logger logger_;
    MessageHandler message_handler_;
    std::unordered_map<int, PublishAcknowledgedHandler> pending_publish_acknowledgements_;
    std::unordered_set<int> early_publish_acknowledgements_;
    bool publish_registration_in_progress_{};
    DisconnectedHandler disconnected_handler_;
    std::optional<MqttWill> will_;
    bool custom_will_{};
    mutable std::mutex subscription_mutex_;
    std::mutex subscription_registration_mutex_;
    std::unordered_map<int, std::pair<std::string, int>> pending_subscriptions_;
    std::unordered_map<int, std::vector<int>> early_subscription_acknowledgements_;
    bool subscription_registration_in_progress_{};
    bool subscriptions_registration_complete_{};
    std::jthread heartbeat_thread_;
    std::atomic_uint64_t server_message_sequence_{};
    std::chrono::steady_clock::time_point started_at_{};
    std::string session_id_;
};

}  // namespace logistics::central_server
