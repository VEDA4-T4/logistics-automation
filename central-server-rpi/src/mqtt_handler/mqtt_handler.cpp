#include "logistics/central_server/mqtt_handler.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "logistics/central_server/device_manager.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;

[[nodiscard]] std::string CurrentIso8601Timestamp() {
    const std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
    {
        static std::mutex time_mutex;
        std::lock_guard lock(time_mutex);
        const std::tm* converted = std::gmtime(&current_time);
        if (converted == nullptr) {
            return {};
        }
        utc_time = *converted;
    }

    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] constexpr bool IsDeviceRegistryMessage(mqtt::MessageType type) noexcept {
    return type == mqtt::MessageType::kDeviceRegister || type == mqtt::MessageType::kHeartbeat ||
           type == mqtt::MessageType::kDeviceStatus || type == mqtt::MessageType::kErrorOccurred;
}

void DefaultLog(MqttHandlerLogLevel level, std::string_view message) {
    const bool is_error = level == MqttHandlerLogLevel::kError;
    std::ostream& output = is_error ? std::cerr : std::clog;
    output << "[server][" << (is_error ? "ERROR" : "INFO") << "] " << message << '\n';
}

}  // namespace

MqttHandler::MqttHandler(DeviceManager& device_manager, Logger logger)
    : device_manager_(device_manager), logger_(logger ? std::move(logger) : Logger(DefaultLog)) {}

bool MqttHandler::Handle(std::string_view topic, std::string_view payload, std::string_view received_at) {
    const auto decoded = mqtt::DeserializeMessage(payload);
    if (!decoded.IsSuccess()) {
        Log(MqttHandlerLogLevel::kError,
            "invalid MQTT JSON; error=" + std::string(mqtt::ToString(decoded.status.error)) +
                "; field=" + decoded.status.field + "; message=" + decoded.status.message);
        return false;
    }

    const auto validation = mqtt::ValidateTopicMessage(topic, decoded.value);
    if (!validation.IsSuccess()) {
        Log(MqttHandlerLogLevel::kError,
            "MQTT topic/message mismatch; error=" + std::string(mqtt::ToString(validation.error)) +
                "; message=" + validation.message);
        return false;
    }

    const auto parsed_topic = mqtt::ParseTopic(topic);
    if (IsDeviceRegistryMessage(decoded.value.message_type)) {
        const std::string effective_received_at =
            received_at.empty() ? CurrentIso8601Timestamp() : std::string(received_at);
        if (!device_manager_.HandleMessage(parsed_topic, decoded.value, effective_received_at)) {
            Log(MqttHandlerLogLevel::kError, "device registry update failed: " + device_manager_.LastError());
            return false;
        }
    }

    Log(MqttHandlerLogLevel::kInfo, "MQTT message received: " + std::string(topic));
    if (decoded.value.message_type == mqtt::MessageType::kDeviceRegister) {
        Log(MqttHandlerLogLevel::kInfo,
            "device registered: " + std::string(parsed_topic.endpoint_id) +
                "; registered devices=" + std::to_string(device_manager_.RegisteredDeviceCount()));
    }
    return true;
}

void MqttHandler::Log(MqttHandlerLogLevel level, std::string_view message) const {
    if (logger_) {
        logger_(level, message);
    }
}

}  // namespace logistics::central_server
