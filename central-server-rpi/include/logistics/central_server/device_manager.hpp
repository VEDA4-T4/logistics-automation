#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {

struct DeviceSnapshot final {
    std::string device_id;
    std::string device_type;
    std::string node_name;
    std::string ip_address;
    contracts::mqtt::ConnectionState connection_state{ contracts::mqtt::ConnectionState::kUnknown };
    std::string current_state;
    std::optional<std::string> job_id;
    std::optional<std::string> error_code;
    std::string last_message_timestamp;
    std::uint64_t uptime{};
    bool uart_connected{};
    bool registered{};
};

class DeviceManager final {
public:
    void HandleMessage(const contracts::mqtt::ParsedTopic& topic, const contracts::mqtt::MqttMessage& message);

    [[nodiscard]] std::size_t RegisteredDeviceCount() const;
    [[nodiscard]] std::optional<DeviceSnapshot> FindDevice(std::string_view device_id) const;

private:
    [[nodiscard]] DeviceSnapshot& FindOrCreateDevice(std::string_view device_id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, DeviceSnapshot> devices_;
};

}  // namespace logistics::central_server
