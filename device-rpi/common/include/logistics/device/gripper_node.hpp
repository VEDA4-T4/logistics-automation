#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/uart/gripper_commands.h"
#include "logistics/device/gripper_pose_config.hpp"
#include "logistics/device/input_uart_session.hpp"

namespace logistics::device {

enum class GripperCommandStatus {
    kAccepted,          // motion started; completion arrives asynchronously
    kSuccess,           // command finished within the UART transaction
    kDuplicate,         // same request or same work is already running
    kActiveCycleConflict,
    kNotHomed,
    kRejected,
    kControllerFailure,
    kTimeout,
    kUnsupportedMessage,
    kUnsupportedCommand,
    kInvalidMessage,
    kInvalidTarget,
    kInvalidParameters,
    kUartNotOpen,
    kUartError,
};

struct GripperCommandResult {
    GripperCommandStatus status{ GripperCommandStatus::kInvalidMessage };
    contracts::mqtt::ControlCommand mqtt_command{ contracts::mqtt::ControlCommand::kUnknown };
    std::string request_id{};
    std::string work_id{};
    std::uint8_t uart_command{};
    std::uint16_t motion_id{ UART_GRIPPER_MOTION_ID_NONE };
    InputTransactResult uart_result{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == GripperCommandStatus::kAccepted || status == GripperCommandStatus::kSuccess;
    }
};

enum class GripperReportChannel {
    kResponse,
    kStatus,
    kEvent,
    kError,
};

struct GripperReport {
    GripperReportChannel channel{ GripperReportChannel::kStatus };
    contracts::mqtt::MessageType message_type{ contracts::mqtt::MessageType::kUnknown };
    contracts::mqtt::MessagePayload data;
};

using GripperReportHandler = std::function<void(const GripperReport& report)>;

/*
 * Steps of one pick-and-place cycle.
 *
 * Every step maps to exactly one asynchronous controller motion. The node sends
 * the motion, waits for the matching MOTION_COMPLETE event, and only then moves
 * to the next step, because the servos have no position feedback and the
 * controller reports completion on a timer.
 */
enum class GripperCycleStep {
    kIdle,
    kOpenClaw,
    kPickApproach,
    kPickDescend,
    kCloseClaw,
    kPickRetreat,
    kPlaceApproach,
    kPlaceDescend,
    kReleaseClaw,
    kPlaceRetreat,
    kReturnHome,
    kCompleted,
};

/*
 * Which part of a cycle a single MQTT command should run.
 *
 * The central server dispatches one START for the whole transfer, but the
 * individual phases stay addressable through component_id so the arm can be
 * exercised step by step during bring-up.
 */
enum class GripperPhase {
    kFullCycle,
    kPick,
    kTransfer,
    kPlace,
    kHome,
};

[[nodiscard]] std::string_view ToString(GripperCycleStep step) noexcept;

/*
 * Translates validated MQTT commands into gripper-controller UART commands and
 * turns the controller's asynchronous motion, safety and heartbeat frames back
 * into MQTT reports.
 *
 * Unlike the conveyor nodes, almost nothing here finishes inside the UART
 * transaction: MOVE_ARM/SET_GRIPPER/HOME answer with RESPONSE(SUCCESS) meaning
 * "accepted", and the real completion arrives later as an EVENT carrying the
 * motion ID. This class therefore owns a small sequencer that walks one cycle
 * forward on each MOTION_COMPLETE and aborts it on FAULT, timeout or E-Stop.
 */
class GripperNode final {
public:
    GripperNode(std::string device_id, InputUartSession& uart_session, GripperPoseConfig poses);

    void SetReportHandler(GripperReportHandler handler);

    [[nodiscard]] GripperCommandResult HandleMqttCommand(const contracts::mqtt::MqttMessage& message);
    void HandleUartFrame(const uart_frame_t& frame);
    void Tick(std::chrono::milliseconds elapsed);
    void ResetControllerHeartbeatMonitor() noexcept;

    [[nodiscard]] bool HasActiveCycle() const noexcept;
    [[nodiscard]] std::string_view ActiveWorkId() const noexcept;
    [[nodiscard]] GripperCycleStep ActiveStep() const noexcept;
    [[nodiscard]] bool IsHomed() const noexcept;

private:
    struct ActiveCycle {
        bool active{};
        GripperPhase phase{ GripperPhase::kFullCycle };
        GripperCycleStep step{ GripperCycleStep::kIdle };
        std::string work_id;
        std::string request_id;
        std::string destination;
        GripperPose pick_pose;
        std::uint16_t motion_id{ UART_GRIPPER_MOTION_ID_NONE };
        std::uint8_t motion_type{};
        std::chrono::milliseconds waited{};
        std::chrono::milliseconds motion_budget{};
    };

    enum class PendingSafetyEvent {
        kNone,
        kEstopLatched,
        kReleased,
    };

    struct PendingSafetyCommand {
        bool active{};
        PendingSafetyEvent expected{ PendingSafetyEvent::kNone };
        contracts::mqtt::ControlCommand command{ contracts::mqtt::ControlCommand::kUnknown };
        std::string request_id;
        std::chrono::milliseconds elapsed{};
    };

    struct HeartbeatState {
        std::uint8_t device_state{};
        std::uint8_t error_code{};
    };

    [[nodiscard]] GripperCommandResult HandleControlCommand(const contracts::mqtt::ControlCommandPayload& command);
    [[nodiscard]] GripperCommandResult HandleEmergencyStop(const contracts::mqtt::EmergencyStopPayload& command);

    [[nodiscard]] GripperCommandResult StartCycle(const contracts::mqtt::ControlCommandPayload& command,
                                                  GripperPhase phase);
    [[nodiscard]] GripperCommandResult RunStop(const contracts::mqtt::ControlCommandPayload& command);
    [[nodiscard]] GripperCommandResult RunInitialize(const contracts::mqtt::ControlCommandPayload& command);
    [[nodiscard]] GripperCommandResult RunRecovery(const contracts::mqtt::ControlCommandPayload& command);
    [[nodiscard]] GripperCommandResult RunStatusRequest(const contracts::mqtt::ControlCommandPayload& command);

    // Sequencer.
    [[nodiscard]] static GripperCycleStep FirstStepOf(GripperPhase phase) noexcept;
    [[nodiscard]] static GripperCycleStep NextStep(GripperPhase phase, GripperCycleStep step) noexcept;
    [[nodiscard]] bool DispatchStep(GripperCommandResult& result);
    void AdvanceCycle();
    void AbortCycle(std::string error_code, std::string message);
    void FinishCycle();

    // UART helpers.
    [[nodiscard]] GripperCommandResult Execute(GripperCommandResult result, std::uint8_t command,
                                               std::span<const std::uint8_t> payload);
    // EMERGENCY_STOP and RESET_DEVICE are answered by a safety broadcast rather
    // than a sequence-matched reply, so they are written once and never retried.
    [[nodiscard]] GripperCommandResult ExecuteAsync(GripperCommandResult result, std::uint8_t command,
                                                    std::span<const std::uint8_t> payload);
    [[nodiscard]] std::uint16_t AllocateMotionId() noexcept;

    // Frame decoding. MOTION_COMPLETE and HEARTBEAT share event ID 0x01, and
    // FAULT and SAFETY share 0x02, so every branch checks the payload length too.
    void HandleControllerEvent(const uart_frame_t& frame);
    void HandleMotionComplete(const uart_frame_t& frame);
    void HandleMotionFault(const uart_frame_t& frame);
    void HandleSafetyEvent(const uart_frame_t& frame);
    void HandleControllerHeartbeat(const uart_frame_t& frame);
    void HandleDeviceStatus(const uart_frame_t& frame);
    void EmitControllerStatus(const uart_frame_t& response);

    // Reporting.
    void EmitCommandResponse(const GripperCommandResult& result, std::string message) const;
    void EmitCommandResponse(std::string request_id, contracts::mqtt::ControlCommand command,
                             contracts::mqtt::CommandResult result, std::optional<std::string> error_code,
                             std::string message) const;
    void EmitCycleStatus(std::string current_state) const;
    void EmitDeviceStatus(std::string current_state, std::optional<std::string> error_code = std::nullopt,
                          std::optional<std::string> job_id = std::nullopt) const;
    void EmitError(std::string error_code, std::string error_level, std::string message,
                   std::optional<std::string> job_id) const;
    void EmitReport(GripperReport report) const;

    [[nodiscard]] bool IsTargetedToThisNode(std::string_view target_device_id) const noexcept;
    [[nodiscard]] static std::optional<GripperPhase> PhaseFromComponent(std::string_view component_id) noexcept;

    std::string device_id_;
    InputUartSession& uart_session_;
    GripperPoseConfig poses_;
    GripperReportHandler report_handler_;

    ActiveCycle cycle_;
    PendingSafetyCommand pending_safety_;
    std::uint16_t next_motion_id_{ UART_GRIPPER_MOTION_ID_MIN };
    bool homed_{ false };
    bool estop_latched_{ false };

    std::optional<HeartbeatState> last_heartbeat_state_;
    std::chrono::milliseconds since_controller_heartbeat_{};
    bool controller_heartbeat_lost_{ false };

    // Last response per request ID, replayed when the server retries a request.
    std::string last_request_id_;
    std::optional<GripperCommandResult> last_result_;
    std::string last_result_message_;
};

}  // namespace logistics::device
