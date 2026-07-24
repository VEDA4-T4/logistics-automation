#include "logistics/device/device_status.hpp"

#include <stdexcept>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::device {

namespace mqtt = contracts::mqtt;

DeviceStatus::DeviceStatus(std::string device_id) : device_id_(std::move(device_id)) {
    if (!mqtt::IsValidTopicLevel(device_id_)) {
        throw std::invalid_argument("device ID must be one non-wildcard MQTT topic level");
    }
}

void DeviceStatus::SetConnectionState(mqtt::ConnectionState state) {
    if (state == mqtt::ConnectionState::kUnknown) {
        throw std::invalid_argument("device connection state cannot be UNKNOWN");
    }
    std::lock_guard lock(mutex_);
    connection_state_ = state;
}

void DeviceStatus::SetCurrentState(std::string current_state) {
    if (current_state.empty()) {
        throw std::invalid_argument("device current state cannot be empty");
    }
    std::lock_guard lock(mutex_);
    current_state_ = std::move(current_state);
}

void DeviceStatus::SetJobId(std::optional<std::string> job_id) {
    if (job_id.has_value() && !mqtt::IsValidTopicLevel(*job_id)) {
        throw std::invalid_argument("job ID must be one non-wildcard MQTT topic level");
    }
    std::lock_guard lock(mutex_);
    job_id_ = std::move(job_id);
}

void DeviceStatus::SetErrorCode(std::optional<std::string> error_code) {
    if (error_code.has_value() && error_code->empty()) {
        throw std::invalid_argument("device error code cannot be empty");
    }
    std::lock_guard lock(mutex_);
    error_code_ = std::move(error_code);
}

void DeviceStatus::SetUartConnected(bool connected) {
    std::lock_guard lock(mutex_);
    uart_connected_ = connected;
}

void DeviceStatus::MarkCommunication(std::string timestamp) {
    if (timestamp.empty()) {
        throw std::invalid_argument("communication timestamp cannot be empty");
    }
    std::lock_guard lock(mutex_);
    last_communication_timestamp_ = std::move(timestamp);
}

DeviceStatusSnapshot DeviceStatus::Snapshot() const {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_at_).count();
    return {
        .device_id = device_id_,
        .connection_state = connection_state_,
        .current_state = current_state_,
        .job_id = job_id_,
        .error_code = error_code_,
        .last_communication_timestamp = last_communication_timestamp_,
        .uptime = static_cast<std::uint64_t>(uptime),
        .uart_connected = uart_connected_,
    };
}

bool DeviceStatus::IsForDevice(std::string_view device_id) const {
    std::lock_guard lock(mutex_);
    return device_id_ == device_id;
}

}  // namespace logistics::device
