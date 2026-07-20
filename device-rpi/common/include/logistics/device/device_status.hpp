#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::device {

struct DeviceStatusSnapshot final {
    std::string device_id;
    contracts::mqtt::ConnectionState connection_state{ contracts::mqtt::ConnectionState::kOffline };
    std::string current_state;
    std::optional<std::string> job_id;
    std::optional<std::string> error_code;
    std::string last_communication_timestamp;
    std::uint64_t uptime{};
    bool uart_connected{};
};

class DeviceStatus final {
public:
    explicit DeviceStatus(std::string device_id);

    void SetConnectionState(contracts::mqtt::ConnectionState state);
    void SetCurrentState(std::string current_state);
    void SetJobId(std::optional<std::string> job_id);
    void SetErrorCode(std::optional<std::string> error_code);
    void SetUartConnected(bool connected);
    void MarkCommunication(std::string timestamp);

    [[nodiscard]] DeviceStatusSnapshot Snapshot() const;
    [[nodiscard]] bool IsForDevice(std::string_view device_id) const;

private:
    std::string device_id_;
    contracts::mqtt::ConnectionState connection_state_{ contracts::mqtt::ConnectionState::kOffline };
    std::string current_state_{ "IDLE" };
    std::optional<std::string> job_id_;
    std::optional<std::string> error_code_;
    std::string last_communication_timestamp_;
    std::chrono::steady_clock::time_point started_at_{ std::chrono::steady_clock::now() };
    bool uart_connected_{};
    mutable std::mutex mutex_;
};

}  // namespace logistics::device
