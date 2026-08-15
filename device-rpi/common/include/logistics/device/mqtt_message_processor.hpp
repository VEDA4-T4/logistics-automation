#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::device {

struct IncomingMqttMessage final {
    contracts::mqtt::MqttMessage message;
    std::string error;
    bool duplicate{ false };

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error.empty();
    }
};

class MqttMessageProcessor final {
public:
    explicit MqttMessageProcessor(std::string device_id);

    [[nodiscard]] IncomingMqttMessage DecodeCommand(std::string_view topic, std::string_view payload);
    void ForgetCommand(std::string_view message_id);

    [[nodiscard]] contracts::mqtt::EncodeResult EncodeHeartbeat(
        std::string message_id, std::string timestamp, std::string current_state, std::uint64_t uptime,
        std::optional<std::string> job_id = std::nullopt, std::optional<std::string> error_code = std::nullopt) const;

    [[nodiscard]] contracts::mqtt::EncodeResult EncodeDeviceRegistration(std::string message_id, std::string timestamp,
                                                                         std::string device_type, std::string node_name,
                                                                         std::string ip_address,
                                                                         bool uart_connected) const;

    [[nodiscard]] contracts::mqtt::EncodeResult EncodeOnlineStatus(std::string message_id, std::string timestamp,
                                                                   std::string current_state = "IDLE") const;

    [[nodiscard]] contracts::mqtt::EncodeResult EncodeOfflineStatus(std::string message_id,
                                                                    std::string timestamp) const;

    [[nodiscard]] contracts::mqtt::EncodeResult EncodeDeviceEvent(const contracts::mqtt::MqttMessage& message) const;
    [[nodiscard]] contracts::mqtt::EncodeResult EncodeDeviceError(const contracts::mqtt::MqttMessage& message) const;
    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> PrepareOutboundMessage(
        const contracts::mqtt::MqttMessage& message) const;

    void RememberCommandResponse(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> CachedCommandResponse(
        const contracts::mqtt::MqttMessage& command) const;

private:
    static constexpr std::size_t kRecentMessageLimit = 256;

    std::string device_id_;
    std::mutex recent_messages_mutex_;
    std::unordered_map<std::string, std::string> recent_messages_;
    std::deque<std::string> recent_message_order_;
    mutable std::mutex response_cache_mutex_;
    std::unordered_map<std::string, contracts::mqtt::MqttMessage> response_cache_;
    std::deque<std::string> response_cache_order_;
    mutable std::mutex epoch_mutex_;
    std::optional<std::string> active_process_epoch_;
    std::unordered_map<std::string, std::string> request_process_epochs_;
    std::deque<std::string> request_epoch_order_;
};

}  // namespace logistics::device
