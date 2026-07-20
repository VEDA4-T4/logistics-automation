#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
    std::string last_heartbeat_timestamp;
    std::string last_seen_timestamp;
    std::optional<std::string> disconnected_at;
    std::string last_message_id;
    std::uint64_t uptime{};
    bool uart_connected{};
    bool registered{};
};

class DeviceRegistryError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DeviceManager final {
public:
    explicit DeviceManager(std::filesystem::path registry_path = {});

    [[nodiscard]] bool HandleMessage(const contracts::mqtt::ParsedTopic& topic,
                                     const contracts::mqtt::MqttMessage& message, std::string_view received_at = {});

    [[nodiscard]] std::size_t RegisteredDeviceCount() const;
    [[nodiscard]] std::optional<DeviceSnapshot> FindDevice(std::string_view device_id) const;
    [[nodiscard]] std::vector<DeviceSnapshot> RegisteredDevices() const;
    [[nodiscard]] std::string LastError() const;

private:
    [[nodiscard]] DeviceSnapshot& FindOrCreateDevice(std::string_view device_id);
    void LoadRegistry();
    [[nodiscard]] bool PersistRegistryLocked();

    std::filesystem::path registry_path_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, DeviceSnapshot> devices_;
    std::string last_error_;
};

}  // namespace logistics::central_server
