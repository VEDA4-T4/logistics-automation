#include "logistics/central_server/device_manager.hpp"

#include <utility>

namespace logistics::central_server {

namespace mqtt = contracts::mqtt;

DeviceSnapshot& DeviceManager::FindOrCreateDevice(std::string_view device_id) {
    auto [iterator, inserted] = devices_.try_emplace(std::string(device_id));
    if (inserted) {
        iterator->second.device_id = device_id;
    }
    return iterator->second;
}

void DeviceManager::HandleMessage(const mqtt::ParsedTopic& topic, const mqtt::MqttMessage& message) {
    if (topic.endpoint_id.empty()) {
        return;
    }

    std::lock_guard lock(mutex_);
    DeviceSnapshot& device = FindOrCreateDevice(topic.endpoint_id);
    device.last_message_timestamp = message.timestamp;

    switch (message.message_type) {
        case mqtt::MessageType::kDeviceRegister: {
            const auto* payload = mqtt::GetPayload<mqtt::DeviceRegisterPayload>(message);
            if (payload == nullptr) {
                return;
            }
            device.device_type = payload->device_type;
            device.node_name = payload->node_name;
            device.ip_address = payload->ip_address;
            device.connection_state = payload->status;
            device.uart_connected = payload->uart_connected;
            device.registered = true;
            break;
        }
        case mqtt::MessageType::kHeartbeat: {
            const auto* payload = mqtt::GetPayload<mqtt::HeartbeatPayload>(message);
            if (payload == nullptr) {
                return;
            }
            device.connection_state = payload->status;
            device.current_state = payload->current_state;
            device.uptime = payload->uptime;
            device.job_id = payload->job_id;
            device.error_code = payload->error_code;
            break;
        }
        case mqtt::MessageType::kDeviceStatus: {
            const auto* payload = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message);
            if (payload == nullptr) {
                return;
            }
            device.connection_state = payload->status;
            device.current_state = payload->current_state;
            device.job_id = payload->job_id;
            device.error_code = payload->error_code;
            break;
        }
        case mqtt::MessageType::kErrorOccurred: {
            const auto* payload = mqtt::GetPayload<mqtt::ErrorOccurredPayload>(message);
            if (payload == nullptr) {
                return;
            }
            device.current_state = payload->current_state;
            device.job_id = payload->job_id;
            device.error_code = payload->error_code;
            break;
        }
        default:
            break;
    }
}

std::size_t DeviceManager::RegisteredDeviceCount() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : devices_) {
        if (entry.second.registered) {
            ++count;
        }
    }
    return count;
}

std::optional<DeviceSnapshot> DeviceManager::FindDevice(std::string_view device_id) const {
    std::lock_guard lock(mutex_);
    const auto iterator = devices_.find(std::string(device_id));
    if (iterator == devices_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

}  // namespace logistics::central_server
