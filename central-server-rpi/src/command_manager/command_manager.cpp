#include "logistics/central_server/command_manager.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;
constexpr std::size_t kCompletedRequestLimit = 256;
constexpr auto kExecuteCompletionTimeout = std::chrono::seconds{ 60 };

[[nodiscard]] std::int64_t UnixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool IsReachable(const DeviceSnapshot& device) noexcept {
    return device.registered && device.connection_state != mqtt::ConnectionState::kOffline;
}

[[nodiscard]] std::optional<std::pair<std::string, mqtt::ControlCommand>> CommandIdentity(
    const mqtt::MqttMessage& message) {
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        return std::pair{ command->request_id, command->command };
    }
    if (const auto* emergency_stop = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message)) {
        return std::pair{ emergency_stop->request_id, emergency_stop->command };
    }
    if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        return std::pair{ destination->request_id, destination->command };
    }
    return std::nullopt;
}

}  // namespace

CommandRoutePlan ResolveCommandTargets(const mqtt::MqttMessage& message,
                                       const std::vector<DeviceSnapshot>& registered_devices) {
    CommandRoutePlan plan;
    std::string_view requested_target;
    bool system_destination_request = false;

    if (const auto* emergency_stop = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message)) {
        static_cast<void>(emergency_stop);
        requested_target = "ALL";
        plan.broadcast = true;
    } else if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
        requested_target = command->target_device_id;
    } else if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message)) {
        requested_target = destination->target_device_id;
        system_destination_request = requested_target == "SYSTEM";
    } else {
        return plan;
    }

    for (const auto& device : registered_devices) {
        if (!IsReachable(device)) {
            continue;
        }
        if (system_destination_request && device.device_type != "linetracer") {
            continue;
        }
        if (requested_target != "SYSTEM" && requested_target != "ALL" && requested_target != device.device_id) {
            continue;
        }
        plan.target_device_ids.push_back(device.device_id);
    }

    std::ranges::sort(plan.target_device_ids);
    return plan;
}

mqtt::MqttMessage PrepareCommandForDevice(const mqtt::MqttMessage& message, std::string_view device_id,
                                          std::string_view line_tracer_device_id,
                                          std::string_view line_tracer_initial_position) {
    auto forwarded = message;
    forwarded.message_id += "-" + std::string(device_id);

    if (auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(forwarded)) {
        command->target_device_id = device_id;
        if ((command->command == mqtt::ControlCommand::kInitialize ||
             command->command == mqtt::ControlCommand::kRecovery) &&
            device_id == line_tracer_device_id && !line_tracer_initial_position.empty()) {
            command->params["currentPosition"] = std::string(line_tracer_initial_position);
        }
    } else if (auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(forwarded)) {
        destination->target_device_id = device_id;
    }

    return forwarded;
}

CommandManager::CommandManager(NowProvider now_provider)
    : now_provider_(now_provider ? std::move(now_provider) : NowProvider([] { return Clock::now(); })) {}

bool CommandManager::TrackCommand(const mqtt::MqttMessage& message, const std::vector<std::string>& target_device_ids) {
    const auto identity = CommandIdentity(message);
    if (!identity || identity->first.empty() || target_device_ids.empty()) {
        std::lock_guard lock(mutex_);
        last_error_ = "command or target device list is invalid";
        return false;
    }

    PendingCommand pending{
        .command = identity->second,
        .original_message = message,
        .started_at = now_provider_(),
        .timeout = mqtt::CommandResponseTimeout(identity->second),
        .expected_devices = {},
        .completed_devices = {},
        .response_message_ids = {},
        .failure = std::nullopt,
        .deadline_at_ms = UnixMilliseconds() + std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   mqtt::CommandResponseTimeout(identity->second))
                                                   .count(),
    };
    pending.expected_devices.insert(target_device_ids.begin(), target_device_ids.end());
    if (pending.expected_devices.empty()) {
        std::lock_guard lock(mutex_);
        last_error_ = "command target device list is empty";
        return false;
    }

    std::lock_guard lock(mutex_);
    if (pending_.contains(identity->first) || completed_requests_.contains(identity->first)) {
        last_error_ = "requestId was already received";
        return false;
    }
    pending_.emplace(identity->first, std::move(pending));
    last_error_.clear();
    return true;
}

std::optional<mqtt::MqttMessage> CommandManager::HandleDispatchFailures(
    std::string_view request_id, const std::vector<std::string>& failed_device_ids, std::string_view timestamp) {
    std::lock_guard lock(mutex_);
    const auto pending_iterator = pending_.find(std::string(request_id));
    if (pending_iterator == pending_.end() || failed_device_ids.empty()) {
        return std::nullopt;
    }

    PendingCommand& pending = pending_iterator->second;
    std::size_t newly_failed = 0;
    for (const auto& device_id : failed_device_ids) {
        if (!pending.expected_devices.contains(device_id)) {
            continue;
        }
        newly_failed += pending.completed_devices.insert(device_id).second ? 1U : 0U;
    }
    if (newly_failed == 0) {
        return std::nullopt;
    }

    pending.failure = mqtt::CommandResponsePayload{
        .request_id = std::string(request_id),
        .command = pending.command,
        .result = mqtt::CommandResult::kFailed,
        .error_code = std::string("ERR-COMMAND-DISPATCH"),
        .message = "command dispatch failed for one or more target devices",
    };

    if (pending.completed_devices.size() < pending.expected_devices.size()) {
        return MakeAggregateResponse(request_id, pending, mqtt::CommandResult::kProcessing, std::string(timestamp),
                                     std::string("ERR-COMMAND-DISPATCH"),
                                     "dispatch partially failed; waiting for remaining device responses");
    }

    auto failed =
        MakeAggregateResponse(request_id, pending, mqtt::CommandResult::kFailed, std::string(timestamp),
                              std::string("ERR-COMMAND-DISPATCH"), "command dispatch failed for all target devices");
    RememberCompletedRequest(std::string(request_id));
    pending_.erase(pending_iterator);
    return failed;
}

std::optional<mqtt::MqttMessage> CommandManager::MakeImmediateResult(const mqtt::MqttMessage& command,
                                                                     mqtt::CommandResult result, std::string timestamp,
                                                                     std::optional<std::string> error_code,
                                                                     std::string message) {
    const auto identity = CommandIdentity(command);
    if (!identity) {
        return std::nullopt;
    }

    std::lock_guard lock(mutex_);
    PendingCommand immediate{
        .command = identity->second,
        .original_message = command,
        .started_at = now_provider_(),
        .timeout = {},
        .expected_devices = {},
        .completed_devices = {},
        .response_message_ids = {},
        .failure = std::nullopt,
    };
    RememberCompletedRequest(identity->first);
    return MakeAggregateResponse(identity->first, immediate, result, std::move(timestamp), std::move(error_code),
                                 std::move(message));
}

CommandResponseDecision CommandManager::HandleResponse(const mqtt::MqttMessage& message) {
    const auto* response = mqtt::GetPayload<mqtt::CommandResponsePayload>(message);
    if (response == nullptr) {
        return {
            .disposition = CommandResponseDisposition::kRejected,
            .message = std::nullopt,
            .reason = "message is not COMMAND_RESPONSE",
        };
    }

    std::lock_guard lock(mutex_);
    const auto pending_iterator = pending_.find(response->request_id);
    if (pending_iterator == pending_.end()) {
        return {
            .disposition = CommandResponseDisposition::kUnknownRequest,
            .message = std::nullopt,
            .reason = "requestId is not pending",
        };
    }

    PendingCommand& pending = pending_iterator->second;
    if (pending.command != response->command || !pending.expected_devices.contains(message.source_id)) {
        return {
            .disposition = CommandResponseDisposition::kRejected,
            .message = std::nullopt,
            .reason = "response command or source device does not match the pending request",
        };
    }
    if (!pending.response_message_ids.insert(message.message_id).second ||
        pending.completed_devices.contains(message.source_id)) {
        return {
            .disposition = CommandResponseDisposition::kDuplicate,
            .message = std::nullopt,
            .reason = "duplicate command response",
        };
    }

    if (!mqtt::IsTerminal(response->result)) {
        if (pending.command == mqtt::ControlCommand::kExecute && response->result == mqtt::CommandResult::kProcessing) {
            pending.started_at = now_provider_();
            pending.timeout = kExecuteCompletionTimeout;
            pending.deadline_at_ms =
                UnixMilliseconds() +
                std::chrono::duration_cast<std::chrono::milliseconds>(kExecuteCompletionTimeout).count();
        }
        return {
            .disposition = CommandResponseDisposition::kForward,
            .message = message,
            .reason = {},
        };
    }

    pending.completed_devices.insert(message.source_id);
    if (response->result != mqtt::CommandResult::kSuccess && response->result != mqtt::CommandResult::kDuplicated &&
        !pending.failure.has_value()) {
        pending.failure = *response;
    }

    if (pending.completed_devices.size() < pending.expected_devices.size()) {
        return {
            .disposition = CommandResponseDisposition::kForward,
            .message = MakeAggregateResponse(response->request_id, pending, mqtt::CommandResult::kProcessing,
                                             message.timestamp, std::nullopt, "waiting for remaining device responses"),
            .reason = {},
        };
    }

    const auto final_result = pending.failure ? pending.failure->result : mqtt::CommandResult::kSuccess;
    const auto final_error = pending.failure ? pending.failure->error_code : std::nullopt;
    const std::string final_message =
        pending.failure ? pending.failure->message : "all target devices completed the command";
    auto aggregate = MakeAggregateResponse(response->request_id, pending, final_result, message.timestamp, final_error,
                                           final_message);
    RememberCompletedRequest(response->request_id);
    pending_.erase(pending_iterator);
    return {
        .disposition = CommandResponseDisposition::kForward,
        .message = std::move(aggregate),
        .reason = {},
    };
}

CommandManagerSnapshot CommandManager::Snapshot() const {
    std::lock_guard lock(mutex_);
    CommandManagerSnapshot snapshot{ .pending = {}, .completed_requests = {}, .message_sequence = message_sequence_ };
    snapshot.completed_requests.assign(completed_request_order_.begin(), completed_request_order_.end());
    snapshot.pending.reserve(pending_.size());
    for (const auto& [request_id, pending] : pending_) {
        PendingCommandSnapshot stored{
            .request_id = request_id,
            .original_message = pending.original_message,
            .expected_devices = { pending.expected_devices.begin(), pending.expected_devices.end() },
            .completed_devices = { pending.completed_devices.begin(), pending.completed_devices.end() },
            .response_message_ids = { pending.response_message_ids.begin(), pending.response_message_ids.end() },
            .failure = pending.failure,
            .deadline_at_ms = pending.deadline_at_ms,
        };
        std::ranges::sort(stored.expected_devices);
        std::ranges::sort(stored.completed_devices);
        std::ranges::sort(stored.response_message_ids);
        snapshot.pending.push_back(std::move(stored));
    }
    std::ranges::sort(snapshot.pending, {}, &PendingCommandSnapshot::request_id);
    return snapshot;
}

bool CommandManager::Restore(CommandManagerSnapshot snapshot) {
    std::unordered_map<std::string, PendingCommand> pending;
    const auto now_wall = UnixMilliseconds();
    for (auto& stored : snapshot.pending) {
        const auto identity = CommandIdentity(stored.original_message);
        if (!identity || identity->first != stored.request_id || stored.expected_devices.empty() ||
            stored.deadline_at_ms < 0) {
            return false;
        }
        PendingCommand restored{
            .command = identity->second,
            .original_message = std::move(stored.original_message),
            .started_at = now_provider_(),
            .timeout = std::chrono::seconds((std::max<std::int64_t>(0, stored.deadline_at_ms - now_wall) + 999) / 1000),
            .expected_devices = { stored.expected_devices.begin(), stored.expected_devices.end() },
            .completed_devices = { stored.completed_devices.begin(), stored.completed_devices.end() },
            .response_message_ids = { stored.response_message_ids.begin(), stored.response_message_ids.end() },
            .failure = std::move(stored.failure),
            .deadline_at_ms = stored.deadline_at_ms,
        };
        if (restored.completed_devices.size() > restored.expected_devices.size() ||
            !std::ranges::all_of(
                restored.completed_devices,
                [&restored](const std::string& id) { return restored.expected_devices.contains(id); }) ||
            !pending.emplace(stored.request_id, std::move(restored)).second) {
            return false;
        }
    }
    std::lock_guard lock(mutex_);
    pending_ = std::move(pending);
    completed_requests_.clear();
    completed_request_order_.clear();
    for (auto& request_id : snapshot.completed_requests) {
        RememberCompletedRequest(std::move(request_id));
    }
    message_sequence_ = snapshot.message_sequence;
    last_error_.clear();
    return true;
}

CommandResponseDecision CommandManager::PreviewResponse(const mqtt::MqttMessage& message) const {
    CommandManager preview(now_provider_);
    {
        std::lock_guard lock(mutex_);
        preview.pending_ = pending_;
        preview.completed_requests_ = completed_requests_;
        preview.completed_request_order_ = completed_request_order_;
        preview.message_sequence_ = message_sequence_;
        preview.last_error_ = last_error_;
    }
    return preview.HandleResponse(message);
}

std::vector<mqtt::MqttMessage> CommandManager::CheckTimeouts(std::string_view checked_at) {
    std::lock_guard lock(mutex_);
    std::vector<mqtt::MqttMessage> timed_out;
    const auto now = now_provider_();

    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        PendingCommand& pending = iterator->second;
        if (now - pending.started_at < pending.timeout) {
            ++iterator;
            continue;
        }

        timed_out.push_back(MakeAggregateResponse(iterator->first, pending, mqtt::CommandResult::kTimeout,
                                                  std::string(checked_at), std::string("ERR-COMMAND-TIMEOUT"),
                                                  "one or more target devices did not respond before timeout"));
        RememberCompletedRequest(iterator->first);
        iterator = pending_.erase(iterator);
    }
    return timed_out;
}

std::vector<mqtt::MqttMessage> CommandManager::PreviewTimeouts(std::string_view checked_at) const {
    CommandManager preview(now_provider_);
    {
        std::lock_guard lock(mutex_);
        preview.pending_ = pending_;
        preview.completed_requests_ = completed_requests_;
        preview.completed_request_order_ = completed_request_order_;
        preview.message_sequence_ = message_sequence_;
    }
    return preview.CheckTimeouts(checked_at);
}

std::size_t CommandManager::PendingCount() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

std::string CommandManager::LastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

mqtt::MqttMessage CommandManager::MakeAggregateResponse(std::string_view request_id, const PendingCommand& pending,
                                                        mqtt::CommandResult result, std::string timestamp,
                                                        std::optional<std::string> error_code, std::string message) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "COMMAND-RESULT-" + std::to_string(++message_sequence_),
        .message_type = mqtt::MessageType::kCommandResponse,
        .source_id = "central-server",
        .timestamp = std::move(timestamp),
        .data =
            mqtt::CommandResponsePayload{
                .request_id = std::string(request_id),
                .command = pending.command,
                .result = result,
                .error_code = std::move(error_code),
                .message = std::move(message),
            },
    };
}

void CommandManager::RememberCompletedRequest(std::string request_id) {
    if (!completed_requests_.insert(request_id).second) {
        return;
    }
    completed_request_order_.push_back(std::move(request_id));
    if (completed_request_order_.size() > kCompletedRequestLimit) {
        completed_requests_.erase(completed_request_order_.front());
        completed_request_order_.pop_front();
    }
}

}  // namespace logistics::central_server
