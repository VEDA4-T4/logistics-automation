#include "logistics/central_server/device_manager.hpp"

#include <fstream>
#include <system_error>
#include <utility>

namespace logistics::central_server {

namespace mqtt = contracts::mqtt;

namespace {

constexpr int kRegistryVersion = 1;

[[nodiscard]] mqtt::Json OptionalString(const std::optional<std::string>& value) {
    return value.has_value() ? mqtt::Json(*value) : mqtt::Json(nullptr);
}

[[nodiscard]] std::optional<std::string> ReadOptionalString(const mqtt::Json& object, std::string_view field) {
    const auto iterator = object.find(std::string(field));
    if (iterator == object.end() || iterator->is_null()) {
        return std::nullopt;
    }
    return iterator->get<std::string>();
}

}  // namespace

DeviceManager::DeviceManager(std::filesystem::path registry_path) : registry_path_(std::move(registry_path)) {
    LoadRegistry();
}

DeviceSnapshot& DeviceManager::FindOrCreateDevice(std::string_view device_id) {
    auto [iterator, inserted] = devices_.try_emplace(std::string(device_id));
    if (inserted) {
        iterator->second.device_id = device_id;
    }
    return iterator->second;
}

bool DeviceManager::HandleMessage(const mqtt::ParsedTopic& topic, const mqtt::MqttMessage& message,
                                  std::string_view received_at) {
    if (topic.endpoint_id.empty()) {
        return false;
    }

    std::lock_guard lock(mutex_);
    DeviceSnapshot& device = FindOrCreateDevice(topic.endpoint_id);

    bool handled = true;
    bool should_persist = false;
    switch (message.message_type) {
        case mqtt::MessageType::kDeviceRegister: {
            const auto* payload = mqtt::GetPayload<mqtt::DeviceRegisterPayload>(message);
            if (payload == nullptr) {
                handled = false;
                break;
            }
            device.device_type = payload->device_type;
            device.node_name = payload->node_name;
            device.ip_address = payload->ip_address;
            device.connection_state = payload->status;
            device.uart_connected = payload->uart_connected;
            device.registered = true;
            if (payload->status == mqtt::ConnectionState::kOnline) {
                device.disconnected_at.reset();
            }
            should_persist = true;
            break;
        }
        case mqtt::MessageType::kHeartbeat: {
            const auto* payload = mqtt::GetPayload<mqtt::HeartbeatPayload>(message);
            if (payload == nullptr) {
                handled = false;
                break;
            }
            device.connection_state = payload->status;
            device.current_state = payload->current_state;
            device.uptime = payload->uptime;
            device.job_id = payload->job_id;
            device.error_code = payload->error_code;
            device.last_heartbeat_timestamp = message.timestamp;
            if (payload->status == mqtt::ConnectionState::kOnline) {
                device.disconnected_at.reset();
            }
            break;
        }
        case mqtt::MessageType::kDeviceStatus: {
            const auto* payload = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message);
            if (payload == nullptr) {
                handled = false;
                break;
            }
            device.connection_state = payload->status;
            device.current_state = payload->current_state;
            device.job_id = payload->job_id;
            device.error_code = payload->error_code;
            if (payload->status == mqtt::ConnectionState::kOffline) {
                device.disconnected_at = received_at.empty() ? message.timestamp : std::string(received_at);
            } else if (payload->status == mqtt::ConnectionState::kOnline) {
                device.disconnected_at.reset();
            }
            should_persist = true;
            break;
        }
        case mqtt::MessageType::kErrorOccurred: {
            const auto* payload = mqtt::GetPayload<mqtt::ErrorOccurredPayload>(message);
            if (payload == nullptr) {
                handled = false;
                break;
            }
            device.current_state = payload->current_state;
            device.job_id = payload->job_id;
            device.error_code = payload->error_code;
            should_persist = true;
            break;
        }
        default:
            handled = false;
            break;
    }

    if (!handled) {
        last_error_ = "message payload does not match a device registry event";
        return false;
    }

    device.last_message_timestamp = message.timestamp;
    device.last_seen_timestamp = received_at.empty() ? message.timestamp : std::string(received_at);
    device.last_message_id = message.message_id;
    last_error_.clear();
    return !device.registered || !should_persist || PersistRegistryLocked();
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

std::vector<DeviceSnapshot> DeviceManager::RegisteredDevices() const {
    std::lock_guard lock(mutex_);
    std::vector<DeviceSnapshot> output;
    output.reserve(devices_.size());
    for (const auto& [device_id, device] : devices_) {
        static_cast<void>(device_id);
        if (device.registered) {
            output.push_back(device);
        }
    }
    return output;
}

std::string DeviceManager::LastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

void DeviceManager::LoadRegistry() {
    if (registry_path_.empty()) {
        return;
    }

    try {
        std::error_code exists_error;
        const bool exists = std::filesystem::exists(registry_path_, exists_error);
        if (exists_error) {
            throw DeviceRegistryError("unable to inspect device registry: " + registry_path_.string() + ": " +
                                      exists_error.message());
        }
        if (!exists) {
            return;
        }

        std::ifstream input(registry_path_);
        if (!input) {
            throw DeviceRegistryError("unable to open device registry: " + registry_path_.string());
        }

        const mqtt::Json root = mqtt::Json::parse(input);
        if (!root.is_object() || root.value("version", 0) != kRegistryVersion || !root.contains("devices") ||
            !root.at("devices").is_array()) {
            throw DeviceRegistryError("invalid device registry structure: " + registry_path_.string());
        }

        for (const auto& item : root.at("devices")) {
            DeviceSnapshot device;
            device.device_id = item.at("deviceId").get<std::string>();
            device.device_type = item.at("deviceType").get<std::string>();
            device.node_name = item.at("nodeName").get<std::string>();
            device.ip_address = item.at("ipAddress").get<std::string>();
            device.connection_state = mqtt::ConnectionState::kOffline;
            device.current_state = "DISCONNECTED";
            device.job_id = ReadOptionalString(item, "jobId");
            device.error_code = ReadOptionalString(item, "errorCode");
            device.last_message_timestamp = item.value("lastReportedTimestamp", std::string{});
            device.last_heartbeat_timestamp = item.value("lastHeartbeatTimestamp", std::string{});
            device.last_seen_timestamp = item.value("lastSeenTimestamp", std::string{});
            device.disconnected_at = ReadOptionalString(item, "disconnectedAt");
            device.last_message_id = item.value("lastMessageId", std::string{});
            device.uptime = item.value("uptime", std::uint64_t{});
            device.uart_connected = item.value("uartConnected", false);
            device.registered = true;
            if (!mqtt::IsValidTopicLevel(device.device_id) || device.device_type.empty() || device.node_name.empty() ||
                device.ip_address.empty()) {
                throw DeviceRegistryError("device registry contains an invalid device: " + registry_path_.string());
            }
            const std::string device_id = device.device_id;
            devices_.insert_or_assign(device_id, std::move(device));
        }
    } catch (const DeviceRegistryError&) {
        throw;
    } catch (const std::exception& error) {
        throw DeviceRegistryError("unable to parse device registry " + registry_path_.string() + ": " + error.what());
    }
}

bool DeviceManager::PersistRegistryLocked() {
    if (registry_path_.empty()) {
        return true;
    }

    try {
        mqtt::Json devices = mqtt::Json::array();
        for (const auto& [device_id, device] : devices_) {
            if (!device.registered) {
                continue;
            }
            devices.push_back({
                { "deviceId", device_id },
                { "deviceType", device.device_type },
                { "nodeName", device.node_name },
                { "ipAddress", device.ip_address },
                { "connectionState", std::string(mqtt::ToString(device.connection_state)) },
                { "currentState", device.current_state },
                { "jobId", OptionalString(device.job_id) },
                { "errorCode", OptionalString(device.error_code) },
                { "lastReportedTimestamp", device.last_message_timestamp },
                { "lastHeartbeatTimestamp", device.last_heartbeat_timestamp },
                { "lastSeenTimestamp", device.last_seen_timestamp },
                { "disconnectedAt", OptionalString(device.disconnected_at) },
                { "lastMessageId", device.last_message_id },
                { "uptime", device.uptime },
                { "uartConnected", device.uart_connected },
            });
        }

        const mqtt::Json root = {
            { "version", kRegistryVersion },
            { "devices", std::move(devices) },
        };
        if (!registry_path_.parent_path().empty()) {
            std::filesystem::create_directories(registry_path_.parent_path());
        }

        auto temporary_path = registry_path_;
        temporary_path += ".tmp";
        {
            std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("unable to open temporary registry file");
            }
            output << root.dump(2) << '\n';
            if (!output.good()) {
                throw std::runtime_error("unable to write temporary registry file");
            }
        }

        std::error_code error;
        std::filesystem::rename(temporary_path, registry_path_, error);
        if (error) {
            std::filesystem::remove(registry_path_, error);
            error.clear();
            std::filesystem::rename(temporary_path, registry_path_, error);
        }
        if (error) {
            throw std::runtime_error("unable to replace registry file: " + error.message());
        }
        last_error_.clear();
        return true;
    } catch (const std::exception& error) {
        last_error_ = "unable to persist device registry " + registry_path_.string() + ": " + error.what();
        return false;
    }
}

}  // namespace logistics::central_server
