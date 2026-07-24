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

private:
    static constexpr std::size_t kRecentMessageLimit = 256;

    std::string device_id_;
    std::mutex recent_messages_mutex_;
    std::unordered_map<std::string, std::string> recent_messages_;
    std::deque<std::string> recent_message_order_;
};

}  // namespace logistics::device
