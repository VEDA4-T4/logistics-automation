#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::device {

template <typename Value, typename Predicate>
[[nodiscard]] bool MakeRoomInBoundedQueue(std::deque<Value>& queue, const std::size_t capacity, Predicate can_replace) {
    if (queue.size() < capacity) {
        return true;
    }
    const auto replaceable = std::find_if(queue.begin(), queue.end(), std::move(can_replace));
    if (replaceable == queue.end()) {
        return false;
    }
    queue.erase(replaceable);
    return true;
}

class NodeCommandQueue final {
public:
    explicit NodeCommandQueue(const std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0U) {
            throw std::invalid_argument("node command queue capacity must be positive");
        }
    }

    [[nodiscard]] bool Push(const contracts::mqtt::MqttMessage& message,
                            std::deque<contracts::mqtt::MqttMessage>* preempted_messages = nullptr) {
        std::lock_guard lock(mutex_);
        if (IsEmergencyStop(message)) {
            // Safety commands have their own bounded capacity so a normal-command
            // backlog can never reject the first emergency stop.
            if (emergency_messages_.size() >= capacity_) {
                return false;
            }
            if (preempted_messages != nullptr) {
                preempted_messages->insert(preempted_messages->end(), std::make_move_iterator(messages_.begin()),
                                           std::make_move_iterator(messages_.end()));
            }
            messages_.clear();
            emergency_messages_.push_back(message);
            return true;
        }
        if (emergency_messages_.size() + messages_.size() >= capacity_) {
            return false;
        }
        messages_.push_back(message);
        return true;
    }

    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> TryPopEmergency() {
        std::lock_guard lock(mutex_);
        if (emergency_messages_.empty()) {
            return std::nullopt;
        }
        auto message = std::move(emergency_messages_.front());
        emergency_messages_.pop_front();
        return message;
    }

    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> TryPop() {
        std::lock_guard lock(mutex_);
        if (messages_.empty()) {
            return std::nullopt;
        }
        auto message = std::move(messages_.front());
        messages_.pop_front();
        return message;
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex_);
        return emergency_messages_.size() + messages_.size();
    }

private:
    [[nodiscard]] static bool IsEmergencyStop(const contracts::mqtt::MqttMessage& message) {
        if (contracts::mqtt::GetPayload<contracts::mqtt::EmergencyStopPayload>(message) != nullptr) {
            return true;
        }
        const auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(message);
        return command != nullptr && command->command == contracts::mqtt::ControlCommand::kEmergencyStop;
    }

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<contracts::mqtt::MqttMessage> emergency_messages_;
    std::deque<contracts::mqtt::MqttMessage> messages_;
};

[[nodiscard]] inline std::optional<contracts::mqtt::MqttMessage> MakeTerminalCommandResponse(
    const contracts::mqtt::MqttMessage& command, std::string_view source_id, std::string message_id,
    std::string timestamp, const contracts::mqtt::CommandResult result, std::optional<std::string> error_code,
    std::string message) {
    namespace mqtt = contracts::mqtt;

    std::string request_id;
    mqtt::ControlCommand control_command = mqtt::ControlCommand::kUnknown;
    if (const auto* payload = mqtt::GetPayload<mqtt::DestinationSetPayload>(command); payload != nullptr) {
        request_id = payload->request_id;
        control_command = payload->command;
    } else if (const auto* payload = mqtt::GetPayload<mqtt::ControlCommandPayload>(command); payload != nullptr) {
        request_id = payload->request_id;
        control_command = payload->command;
    } else if (const auto* payload = mqtt::GetPayload<mqtt::EmergencyStopPayload>(command); payload != nullptr) {
        request_id = payload->request_id;
        control_command = payload->command;
    }

    if (request_id.empty() || control_command == mqtt::ControlCommand::kUnknown) {
        return std::nullopt;
    }

    return mqtt::MqttMessage{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kCommandResponse,
        .source_id = std::string(source_id),
        .timestamp = std::move(timestamp),
        .data =
            mqtt::CommandResponsePayload{
                .request_id = std::move(request_id),
                .command = control_command,
                .result = result,
                .error_code = std::move(error_code),
                .message = std::move(message),
            },
    };
}

}  // namespace logistics::device
