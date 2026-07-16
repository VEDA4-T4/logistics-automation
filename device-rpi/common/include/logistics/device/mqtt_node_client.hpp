#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/mqtt_node_config.hpp"

namespace logistics::device {

class MqttNodeClient final {
public:
    using CommandHandler = std::function<void(const contracts::mqtt::MqttMessage& message)>;

    MqttNodeClient(MqttNodeConfig config, std::string device_type);
    ~MqttNodeClient();

    MqttNodeClient(const MqttNodeClient&) = delete;
    MqttNodeClient& operator=(const MqttNodeClient&) = delete;
    MqttNodeClient(MqttNodeClient&&) = delete;
    MqttNodeClient& operator=(MqttNodeClient&&) = delete;

    void SetCommandHandler(CommandHandler handler);

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] bool PublishHeartbeat(std::string message_id, std::string timestamp, std::string current_state,
                                        std::uint64_t uptime);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace logistics::device
