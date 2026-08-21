#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/device_status.hpp"
#include "logistics/device/mqtt_node_config.hpp"

namespace logistics::device {

class MqttNodeClient final {
public:
    using CommandHandler = std::function<bool(const contracts::mqtt::MqttMessage& message)>;

    MqttNodeClient(MqttNodeConfig config, std::string device_type, std::shared_ptr<DeviceStatus> device_status);
    ~MqttNodeClient();

    MqttNodeClient(const MqttNodeClient&) = delete;
    MqttNodeClient& operator=(const MqttNodeClient&) = delete;
    MqttNodeClient(MqttNodeClient&&) = delete;
    MqttNodeClient& operator=(MqttNodeClient&&) = delete;

    void SetCommandHandler(CommandHandler handler);
    void SetWorkCreatedEpochReassignmentGuard(std::function<bool()> guard);

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] bool PublishHeartbeat(std::string message_id, std::string timestamp);
    [[nodiscard]] bool PublishResponse(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] bool PublishStatus(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] bool PublishEvent(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] bool PublishError(const contracts::mqtt::MqttMessage& message);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace logistics::device
