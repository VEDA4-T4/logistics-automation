#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::device {

struct IncomingMqttMessage final {
    contracts::mqtt::MqttMessage message;
    std::string error;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error.empty();
    }
};

class MqttMessageProcessor final {
public:
    explicit MqttMessageProcessor(std::string device_id);

    [[nodiscard]] IncomingMqttMessage DecodeCommand(std::string_view topic, std::string_view payload) const;

    [[nodiscard]] contracts::mqtt::EncodeResult EncodeHeartbeat(
        std::string message_id, std::string timestamp, std::string current_state, std::uint64_t uptime,
        std::optional<std::string> job_id = std::nullopt, std::optional<std::string> error_code = std::nullopt) const;

private:
    std::string device_id_;
};

}  // namespace logistics::device
