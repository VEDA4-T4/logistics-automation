#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "logistics/central_server/device_manager.hpp"
#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

struct CommandRoutePlan final {
    std::vector<std::string> target_device_ids;
    bool broadcast{};

    [[nodiscard]] bool IsValid() const noexcept {
        return !target_device_ids.empty();
    }
};

[[nodiscard]] CommandRoutePlan ResolveCommandTargets(const contracts::mqtt::MqttMessage& message,
                                                     const std::vector<DeviceSnapshot>& registered_devices);

enum class CommandResponseDisposition : std::uint8_t {
    kForward,
    kDuplicate,
    kUnknownRequest,
    kRejected,
};

struct CommandResponseDecision final {
    CommandResponseDisposition disposition{ CommandResponseDisposition::kRejected };
    std::optional<contracts::mqtt::MqttMessage> message;
    std::string reason;
};

class CommandManager final {
public:
    using Clock = std::chrono::steady_clock;
    using NowProvider = std::function<Clock::time_point()>;

    explicit CommandManager(NowProvider now_provider = {});

    [[nodiscard]] bool TrackCommand(const contracts::mqtt::MqttMessage& message,
                                    const std::vector<std::string>& target_device_ids);
    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> HandleDispatchFailures(
        std::string_view request_id, const std::vector<std::string>& failed_device_ids, std::string_view timestamp);
    [[nodiscard]] std::optional<contracts::mqtt::MqttMessage> MakeImmediateResult(
        const contracts::mqtt::MqttMessage& command, contracts::mqtt::CommandResult result, std::string timestamp,
        std::optional<std::string> error_code, std::string message);
    [[nodiscard]] CommandResponseDecision HandleResponse(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] std::vector<contracts::mqtt::MqttMessage> CheckTimeouts(std::string_view checked_at);
    [[nodiscard]] std::size_t PendingCount() const;
    [[nodiscard]] std::string LastError() const;

private:
    struct PendingCommand final {
        contracts::mqtt::ControlCommand command{ contracts::mqtt::ControlCommand::kUnknown };
        Clock::time_point started_at;
        std::chrono::seconds timeout{};
        std::unordered_set<std::string> expected_devices;
        std::unordered_set<std::string> completed_devices;
        std::unordered_set<std::string> response_message_ids;
        std::optional<contracts::mqtt::CommandResponsePayload> failure;
    };

    [[nodiscard]] contracts::mqtt::MqttMessage MakeAggregateResponse(
        std::string_view request_id, const PendingCommand& pending, contracts::mqtt::CommandResult result,
        std::string timestamp, std::optional<std::string> error_code, std::string message);
    void RememberCompletedRequest(std::string request_id);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, PendingCommand> pending_;
    std::unordered_set<std::string> completed_requests_;
    std::deque<std::string> completed_request_order_;
    NowProvider now_provider_;
    std::uint64_t message_sequence_{};
    std::string last_error_;
};

}  // namespace logistics::central_server
