#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/uart/linetracer_commands.h"
#include "logistics/device/uart_session.hpp"

namespace logistics::device {

enum class LineTracerCommandStatus {
    kSent,
    kInvalidMessage,
    kInvalidTarget,
    kInvalidDestination,
    kNoActiveJob,
    kUnsupportedMessage,
    kUnsupportedCommand,
    kUartNotOpen,
    kUartBusy,
    kUartError,
};

struct LineTracerCommandResult {
    LineTracerCommandStatus status{ LineTracerCommandStatus::kInvalidMessage };
    contracts::mqtt::ControlCommand mqtt_command{ contracts::mqtt::ControlCommand::kUnknown };
    std::string request_id;
    std::string work_id;
    std::uint16_t uart_job_id{};
    std::uint8_t uart_route_id{};
    std::uint8_t uart_command{};
    std::uint8_t uart_sequence{};
    UartSessionSendResult uart_result{};

    [[nodiscard]] bool Succeeded() const noexcept;
};

/*
 * Converts validated MQTT commands into line-tracer UART commands. MQTT work
 * IDs are UUID strings, while the STM32 contract uses a local uint16_t job ID;
 * this class owns that active-job mapping.
 */
class LineTracerNode final {
public:
    LineTracerNode(std::string device_id, UartSession& uart_session);

    [[nodiscard]] LineTracerCommandResult HandleMqttCommand(const contracts::mqtt::MqttMessage& message);
    void HandleUartEvent(const UartSessionEvent& event) noexcept;

    [[nodiscard]] bool HasActiveJob() const noexcept;
    [[nodiscard]] std::string_view ActiveWorkId() const noexcept;
    [[nodiscard]] std::uint16_t ActiveUartJobId() const noexcept;
    [[nodiscard]] std::uint8_t ActiveRouteId() const noexcept;

    [[nodiscard]] static std::optional<std::uint8_t> RouteFromDestination(std::string_view destination);

private:
    enum class PendingEffect {
        kNone,
        kActivateJob,
        kClearJob,
    };

    struct PendingContext {
        bool active{};
        PendingEffect effect{ PendingEffect::kNone };
        std::uint8_t sequence{};
        std::string work_id;
        std::uint16_t uart_job_id{};
        std::uint8_t route_id{};
    };

    [[nodiscard]] LineTracerCommandResult HandleDestinationSet(const contracts::mqtt::DestinationSetPayload& command);
    [[nodiscard]] LineTracerCommandResult HandleControlCommand(const contracts::mqtt::ControlCommandPayload& command);
    [[nodiscard]] LineTracerCommandResult Send(LineTracerCommandResult result, std::uint8_t command,
                                               const std::uint8_t* payload, std::size_t payload_length);
    [[nodiscard]] bool IsTargetedToThisNode(std::string_view target_device_id) const noexcept;
    [[nodiscard]] std::uint16_t AllocateJobId() noexcept;
    void RememberPending(PendingEffect effect, const LineTracerCommandResult& result);
    void ClearPending() noexcept;

    std::string device_id_;
    UartSession& uart_session_;
    std::string active_work_id_;
    std::uint16_t active_uart_job_id_{};
    std::uint8_t active_route_id_{};
    std::uint16_t next_uart_job_id_{ UART_LINETRACER_JOB_ID_MIN };
    PendingContext pending_{};
};

}  // namespace logistics::device
