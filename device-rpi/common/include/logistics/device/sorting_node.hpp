#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/uart/sorting_commands.h"
#include "logistics/device/uart_session.hpp"

namespace logistics::device {

enum class SortingCommandStatus {
    kSent,
    kDuplicate,
    kInvalidMessage,
    kInvalidTarget,
    kInvalidDestination,
    kActiveCycleConflict,
    kNoActiveCycle,
    kUnsupportedMessage,
    kUnsupportedCommand,
    kUartNotOpen,
    kUartBusy,
    kUartError,
};

struct SortingCommandResult {
    SortingCommandStatus status{ SortingCommandStatus::kInvalidMessage };
    contracts::mqtt::ControlCommand mqtt_command{ contracts::mqtt::ControlCommand::kUnknown };
    std::string request_id;
    std::string work_id;
    std::uint16_t uart_cycle_id{};
    std::uint8_t uart_destination{};
    std::uint8_t uart_command{};
    std::uint8_t uart_sequence{};
    UartSessionSendResult uart_result{};

    [[nodiscard]] bool Succeeded() const noexcept;
};

enum class SortingReportChannel {
    kResponse,
    kStatus,
    kEvent,
    kError,
};

struct SortingReport {
    SortingReportChannel channel{ SortingReportChannel::kStatus };
    contracts::mqtt::MessageType message_type{ contracts::mqtt::MessageType::kUnknown };
    contracts::mqtt::MessagePayload data;
};

using SortingReportHandler = std::function<void(const SortingReport& report)>;

/*
 * Translates validated MQTT commands into sorting-controller UART commands.
 * The server owns UUID work IDs while the STM32 protocol uses a local uint16_t
 * cycle ID, so this class also owns the active work-to-cycle mapping.
 */
class SortingNode final {
public:
    SortingNode(std::string device_id, UartSession& uart_session);

    void SetReportHandler(SortingReportHandler handler);
    [[nodiscard]] SortingCommandResult HandleMqttCommand(const contracts::mqtt::MqttMessage& message);
    [[nodiscard]] SortingCommandResult RequestControllerStatus();
    void HandleUartEvent(const UartSessionEvent& event) noexcept;

    [[nodiscard]] bool HasActiveCycle() const noexcept;
    [[nodiscard]] std::string_view ActiveWorkId() const noexcept;
    [[nodiscard]] std::uint16_t ActiveCycleId() const noexcept;
    [[nodiscard]] std::uint8_t ActiveDestination() const noexcept;

    [[nodiscard]] static std::optional<std::uint8_t> DestinationFromString(std::string_view destination);

private:
    enum class PendingEffect {
        kNone,
        kActivateCycle,
        kClearCycle,
    };

    struct PendingContext {
        bool active{};
        PendingEffect effect{ PendingEffect::kNone };
        std::uint8_t sequence{};
        std::uint8_t uart_command{};
        contracts::mqtt::ControlCommand mqtt_command{ contracts::mqtt::ControlCommand::kUnknown };
        std::string request_id;
        std::string work_id;
        std::uint16_t uart_cycle_id{};
        std::uint8_t uart_destination{};
    };

    [[nodiscard]] SortingCommandResult HandleDestinationSet(const contracts::mqtt::DestinationSetPayload& command);
    [[nodiscard]] SortingCommandResult HandleControlCommand(const contracts::mqtt::ControlCommandPayload& command);
    [[nodiscard]] SortingCommandResult HandleEmergencyStop(const contracts::mqtt::EmergencyStopPayload& command);
    [[nodiscard]] SortingCommandResult Send(SortingCommandResult result, std::uint8_t command,
                                            const std::uint8_t* payload, std::size_t payload_length);
    [[nodiscard]] bool IsTargetedToThisNode(std::string_view target_device_id) const noexcept;
    [[nodiscard]] std::uint16_t AllocateCycleId() noexcept;
    void RememberPending(PendingEffect effect, const SortingCommandResult& result);
    void ClearPending() noexcept;
    void ClearActiveCycle() noexcept;
    void HandleCommandResponse(const UartSessionEvent& event) noexcept;
    void HandleSortingFrame(const uart_frame_t& frame) noexcept;
    [[nodiscard]] bool HandleStatusResponse(const uart_frame_t& frame) noexcept;
    void HandleCycleComplete(const uart_frame_t& frame) noexcept;
    void HandleHeartbeat(const uart_frame_t& frame) noexcept;
    void HandleSensorStatus(const uart_frame_t& frame) noexcept;
    void HandleDeviceStatus(const uart_frame_t& frame) noexcept;
    void HandleSafetyEvent(const uart_frame_t& frame) noexcept;
    void EmitPendingResponse(contracts::mqtt::CommandResult result, std::optional<std::string> error_code,
                             std::string message) const noexcept;
    void EmitStatus(std::string current_state, std::optional<std::string> error_code = std::nullopt) const noexcept;
    void EmitError(std::string error_code, std::string level, std::string current_state, std::string message,
                   std::optional<std::int32_t> distance = std::nullopt) const noexcept;
    void EmitReport(SortingReport report) const noexcept;

    std::string device_id_;
    UartSession& uart_session_;
    std::string active_work_id_;
    std::uint16_t active_cycle_id_{};
    std::uint8_t active_destination_{};
    std::uint16_t next_cycle_id_{ UART_SORTING_CYCLE_ID_MIN };
    PendingContext pending_{};
    std::array<std::uint8_t, 3U> sensor_states_{ 0xffU, 0xffU, 0xffU };
    std::uint8_t last_device_state_{ 0xffU };
    std::uint8_t last_device_error_{ 0xffU };
    SortingReportHandler report_handler_;
};

}  // namespace logistics::device
