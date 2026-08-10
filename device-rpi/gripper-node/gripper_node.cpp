#include "logistics/device/gripper_node.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;

/*
 * Controller event IDs.
 *
 * The gripper firmware reuses the low event IDs for two unrelated families:
 * APP_EVENT_HEARTBEAT and UART_GRIPPER_EVENT_MOTION_COMPLETE are both 0x01, and
 * APP_EVENT_SAFETY and UART_GRIPPER_EVENT_FAULT are both 0x02. The payload
 * length is what separates them, so every decode below checks the length as
 * well as the ID. Getting this wrong would silently read a heartbeat as a
 * motion completion and advance the cycle early.
 */
constexpr std::uint8_t kAppEventHeartbeat = 0x01U;
constexpr std::uint8_t kAppEventSafety = 0x02U;

constexpr std::size_t kHeartbeatPayloadSize = 7U;  // APP_HEARTBEAT_PAYLOAD_SIZE
constexpr std::size_t kHeartbeatStateIndex = 1U;
constexpr std::size_t kHeartbeatErrorIndex = 2U;

constexpr std::size_t kSafetyEventPayloadSize = 3U;
constexpr std::size_t kSafetyEventLatchedIndex = 1U;
constexpr std::size_t kSafetyEventCauseIndex = 2U;

// A motion may legitimately take its full interpolation time plus controller
// scheduling slack; anything beyond that means the completion event was lost.
constexpr auto kMotionTimeoutSlack = std::chrono::milliseconds{ 2000 };
constexpr auto kSafetyEventTimeout = std::chrono::milliseconds{ 1500 };
constexpr auto kControllerHeartbeatTimeout = std::chrono::milliseconds{ 5000 };

void WriteU16(std::uint8_t* payload, std::size_t low_index, std::uint16_t value) noexcept {
    payload[low_index] = static_cast<std::uint8_t>(value & 0xFFU);
    payload[low_index + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

[[nodiscard]] std::uint16_t ReadU16(const std::uint8_t* payload, std::size_t low_index) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(payload[low_index]) |
                                      (static_cast<std::uint16_t>(payload[low_index + 1U]) << 8U));
}

[[nodiscard]] std::string HexByte(std::uint8_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    return std::string{ '0', 'x', digits[value >> 4U], digits[value & 0x0FU] };
}

[[nodiscard]] std::string DescribeDeviceState(std::uint8_t state) {
    switch (state) {
        case UART_DEVICE_READY:
            return "READY";
        case UART_DEVICE_RUNNING:
            return "RUNNING";
        case UART_DEVICE_STOPPED:
            return "STOPPED";
        case UART_DEVICE_ERROR:
            return "ERROR";
        case UART_DEVICE_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

[[nodiscard]] std::string DescribeGripperState(std::uint8_t state) {
    switch (state) {
        case UART_GRIPPER_STATE_IDLE:
            return "IDLE";
        case UART_GRIPPER_STATE_MOVING_ARM:
            return "MOVING_ARM";
        case UART_GRIPPER_STATE_MOVING_GRIPPER:
            return "MOVING_GRIPPER";
        case UART_GRIPPER_STATE_HOMING:
            return "HOMING";
        case UART_GRIPPER_STATE_STOPPED:
            return "STOPPED";
        case UART_GRIPPER_STATE_FAULT:
            return "FAULT";
        case UART_GRIPPER_STATE_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

[[nodiscard]] std::string DescribeUartError(std::uint8_t error) {
    switch (error) {
        case UART_ERROR_TIMEOUT:
            return "ERR-CONTROLLER-TIMEOUT";
        case UART_ERROR_SERVO:
            return "ERR-GRIPPER-SERVO";
        case UART_ERROR_EMERGENCY_STOP:
            return "ERR-EMERGENCY-STOP";
        case UART_GRIPPER_ERROR_NOT_HOMED:
            return "ERR-GRIPPER-NOT-HOMED";
        default:
            return "ERR-CONTROLLER-INTERNAL";
    }
}

[[nodiscard]] std::optional<std::string> ReadStringParam(const mqtt::Json& params, const char* key) {
    const auto found = params.find(key);
    if (found == params.end() || !found->is_string()) {
        return std::nullopt;
    }
    std::string value = found->get<std::string>();
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

/*
 * Reads one coordinate of params.targetPose.
 *
 * Integers are accepted alongside reals because a target that lands on a whole
 * millimetre serialises as an integer in most JSON writers, and rejecting it
 * would make the contract depend on the server's formatting.
 */
[[nodiscard]] std::optional<double> ReadCoordinate(const mqtt::Json& pose, const char* key) {
    const auto found = pose.find(key);
    if (found == pose.end() || !found->is_number()) {
        return std::nullopt;
    }
    const double value = found->get<double>();
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    // A coordinate this far out is a unit mix-up (metres for millimetres, or a
    // raw pixel value) rather than a reachable point, and saying so is more
    // useful than letting the reach check call it "too far".
    if (std::fabs(value) > 10000.0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::int32_t> ReadIntParam(const mqtt::Json& params, const char* key) {
    const auto found = params.find(key);
    if (found == params.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    const std::int64_t value = found->get<std::int64_t>();
    // A pixel offset far outside this range is a malformed message rather than a
    // real detection, and letting it through would narrow to a bogus angle.
    if (value < -100000 || value > 100000) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

/*
 * GripperPose and JointAngles carry the same three deci-degree values but belong
 * to different layers: one is a taught configuration waypoint, the other is a
 * kinematics result. Converting at the boundary keeps the config API stable for
 * the taught-pose path.
 */
[[nodiscard]] GripperPose ToGripperPose(const JointAngles& angles) noexcept {
    return GripperPose{ angles.base_deci_deg, angles.shoulder_deci_deg, angles.elbow_deci_deg };
}

[[nodiscard]] JointAngles ToJointAngles(const GripperPose& pose) noexcept {
    return JointAngles{ pose.base_deci_deg, pose.shoulder_deci_deg, pose.elbow_deci_deg };
}

struct IkFailure {
    std::string error_code;
    std::string message;
};

/*
 * Maps a solver failure onto an MQTT error the server can act on.
 *
 * The distinctions are kept because the operator response differs: too-far
 * usually means a detection outside the arm's half of the conveyor, too-near
 * means the target is inside the fold radius the two links cannot close past,
 * and a joint limit means the point is reachable but only through a posture the
 * firmware refuses.
 */
[[nodiscard]] IkFailure DescribeIkFailure(const IkSolution& solution) {
    switch (solution.status) {
        case IkStatus::kUnreachableTooFar:
            return IkFailure{ "ERR-GRIPPER-UNREACHABLE-FAR", "pick pose is beyond the arm's reach" };
        case IkStatus::kUnreachableTooClose:
            return IkFailure{ "ERR-GRIPPER-UNREACHABLE-NEAR", "pick pose is inside the arm's minimum fold radius" };
        case IkStatus::kJointLimit:
            return IkFailure{ "ERR-GRIPPER-JOINT-LIMIT", "pick pose needs the " + std::string(solution.blocking_joint) +
                                                             " joint outside its mechanical limit" };
        case IkStatus::kInvalidGeometry:
            return IkFailure{ "ERR-GRIPPER-GEOMETRY", "arm geometry configuration cannot solve a Cartesian target" };
        case IkStatus::kOk:
            break;
    }
    return IkFailure{ "ERR-GRIPPER-POSE-MALFORMED", "pick pose could not be solved" };
}

}  // namespace

std::string_view ToString(GripperCycleStep step) noexcept {
    switch (step) {
        case GripperCycleStep::kIdle:
            return "IDLE";
        case GripperCycleStep::kOpenClaw:
            return "PREPARING";
        case GripperCycleStep::kPickApproach:
        case GripperCycleStep::kPickDescend:
        case GripperCycleStep::kCloseClaw:
            return "PICKING";
        case GripperCycleStep::kPickRetreat:
        case GripperCycleStep::kTransfer:
        case GripperCycleStep::kPlaceApproach:
            return "TRANSFERRING";
        case GripperCycleStep::kPlaceDescend:
        case GripperCycleStep::kReleaseClaw:
        case GripperCycleStep::kPlaceRetreat:
            return "PLACING";
        case GripperCycleStep::kReturnTransfer:
        case GripperCycleStep::kReturnPickSide:
        case GripperCycleStep::kReturnHome:
            return "HOMING";
        case GripperCycleStep::kCompleted:
            return "COMPLETED";
    }
    return "UNKNOWN";
}

GripperNode::GripperNode(std::string device_id, InputUartSession& uart_session, GripperPoseConfig poses)
    : device_id_(std::move(device_id)), uart_session_(uart_session), poses_(std::move(poses)) {}

void GripperNode::SetReportHandler(GripperReportHandler handler) {
    report_handler_ = std::move(handler);
}

bool GripperNode::HasActiveCycle() const noexcept {
    return cycle_.active;
}

std::string_view GripperNode::ActiveWorkId() const noexcept {
    return cycle_.work_id;
}

GripperCycleStep GripperNode::ActiveStep() const noexcept {
    return cycle_.step;
}

bool GripperNode::IsHomed() const noexcept {
    return homed_;
}

bool GripperNode::IsTargetedToThisNode(std::string_view target_device_id) const noexcept {
    return target_device_id == device_id_;
}

std::optional<GripperPhase> GripperNode::PhaseFromComponent(std::string_view component_id) noexcept {
    if (component_id.empty() || component_id == "gripper" || component_id == "arm") {
        return GripperPhase::kFullCycle;
    }
    if (component_id == "pick") {
        return GripperPhase::kPick;
    }
    if (component_id == "transfer") {
        return GripperPhase::kTransfer;
    }
    if (component_id == "place") {
        return GripperPhase::kPlace;
    }
    if (component_id == "home") {
        return GripperPhase::kHome;
    }
    return std::nullopt;
}

std::uint16_t GripperNode::AllocateMotionId() noexcept {
    const std::uint16_t motion_id = next_motion_id_;
    next_motion_id_ = (next_motion_id_ == UART_GRIPPER_MOTION_ID_MAX)
                          ? UART_GRIPPER_MOTION_ID_MIN
                          : static_cast<std::uint16_t>(next_motion_id_ + 1U);
    return motion_id;
}

// ---------------------------------------------------------------------------
// MQTT command entry point
// ---------------------------------------------------------------------------

GripperCommandResult GripperNode::HandleMqttCommand(const mqtt::MqttMessage& message) {
    if (const auto* estop = std::get_if<mqtt::EmergencyStopPayload>(&message.data); estop != nullptr) {
        return HandleEmergencyStop(*estop);
    }

    const auto* command = std::get_if<mqtt::ControlCommandPayload>(&message.data);
    if (command == nullptr) {
        return GripperCommandResult{ .status = GripperCommandStatus::kUnsupportedMessage };
    }
    if (!command->IsValid()) {
        GripperCommandResult result{ .status = GripperCommandStatus::kInvalidMessage,
                                     .mqtt_command = command->command,
                                     .request_id = command->request_id };
        EmitCommandResponse(result, "control command failed contract validation");
        return result;
    }
    if (!IsTargetedToThisNode(command->target_device_id)) {
        return GripperCommandResult{ .status = GripperCommandStatus::kInvalidTarget,
                                     .mqtt_command = command->command,
                                     .request_id = command->request_id };
    }

    // Exact message resends never reach here: MqttNodeClient recognizes a
    // repeated MQTT message ID and replays the cached CommandResponse itself
    // (see HandleMessage in mqtt_node_client.cpp), so this node only needs to
    // reject a genuinely new request for work that is already running, which
    // StartCycle does by comparing work IDs.
    return HandleControlCommand(*command);
}

GripperCommandResult GripperNode::HandleControlCommand(const mqtt::ControlCommandPayload& command) {
    switch (command.command) {
        case mqtt::ControlCommand::kStart:
        case mqtt::ControlCommand::kRestart: {
            GripperCommandResult result{ .status = GripperCommandStatus::kSuccess,
                                         .mqtt_command = command.command,
                                         .request_id = command.request_id };
            if (estop_latched_) {
                result.status = GripperCommandStatus::kRejected;
                EmitCommandResponse(result.request_id, command.command, mqtt::CommandResult::kRejected,
                                    std::string("ERR-EMERGENCY-STOP"),
                                    "emergency stop is latched; send RECOVERY before START");
                return result;
            }
            running_requested_ = true;
            if (!homed_) {
                return RunInitialize(command);
            }
            EmitCommandResponse(result, "gripper entered running state");
            if (!cycle_.active) {
                EmitDeviceStatus("RUNNING");
            }
            return result;
        }

        case mqtt::ControlCommand::kExecute: {
            const auto phase = PhaseFromComponent(command.component_id);
            if (!phase.has_value()) {
                GripperCommandResult result{ .status = GripperCommandStatus::kUnsupportedCommand,
                                             .mqtt_command = command.command,
                                             .request_id = command.request_id };
                EmitCommandResponse(result, "unsupported gripper component: " + command.component_id);
                return result;
            }
            return StartCycle(command, *phase);
        }

        case mqtt::ControlCommand::kStop:
            return RunStop(command);

        case mqtt::ControlCommand::kInitialize:
            running_requested_ = false;
            return RunInitialize(command);

        case mqtt::ControlCommand::kRecovery:
            running_requested_ = false;
            return RunRecovery(command);

        case mqtt::ControlCommand::kStatusRequest:
            return RunStatusRequest(command);

        default: {
            GripperCommandResult result{ .status = GripperCommandStatus::kUnsupportedCommand,
                                         .mqtt_command = command.command,
                                         .request_id = command.request_id };
            EmitCommandResponse(result, "command is not supported by the gripper node");
            return result;
        }
    }
}

GripperCommandResult GripperNode::HandleEmergencyStop(const mqtt::EmergencyStopPayload& command) {
    GripperCommandResult result{ .status = GripperCommandStatus::kUartError,
                                 .mqtt_command = mqtt::ControlCommand::kEmergencyStop,
                                 .request_id = command.request_id };

    result = ExecuteAsync(std::move(result), UART_CMD_EMERGENCY_STOP, {});
    if (result.Succeeded()) {
        running_requested_ = false;
        estop_requested_ = true;
        pending_safety_ = PendingSafetyCommand{ .active = true,
                                                .expected = PendingSafetyEvent::kEstopLatched,
                                                .command = mqtt::ControlCommand::kEmergencyStop,
                                                .request_id = command.request_id,
                                                .elapsed = {} };
        // The controller latches the stop and invalidates the running motion; the
        // cycle is abandoned here so no completion event can revive it.
        if (cycle_.active) {
            AbortCycle("ERR-EMERGENCY-STOP", "cycle aborted by emergency stop");
        }
    } else {
        EmitCommandResponse(result, "emergency stop could not be written to the controller");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Pose resolution
// ---------------------------------------------------------------------------

GripperNode::PoseResolution GripperNode::ResolvePickPoses(const mqtt::Json& params) const {
    // The central server's homography transform names this targetPose and adds
    // rollDeg/pitchDeg/yawDeg plus box/coordinateFrame/unit/calibrationVersion
    // metadata alongside it. Only x/y/z is read: yaw is ignored because the arm
    // has no wrist joint to express it independently of position (see
    // gripper_kinematics.hpp), and roll/pitch don't correspond to anything this
    // arm can do either.
    const auto pose_field = params.find("targetPose");
    if (pose_field == params.end()) {
        // No coordinates: keep working from the waypoints taught against the
        // assembled arm. This is still the accurate path until the link lengths
        // in [gripper] have been measured.
        return PoseResolution{ .ok = true,
                               .pick = poses_.PickPoseForOffset(ReadIntParam(params, "offsetX").value_or(0)),
                               .approach = poses_.pick_approach,
                               .from_kinematics = false,
                               .error_code = {},
                               .message = {} };
    }

    if (!pose_field->is_object()) {
        return PoseResolution{ .ok = false,
                               .error_code = "ERR-GRIPPER-POSE-MALFORMED",
                               .message = "params.targetPose must be an object with x, y and z in millimetres" };
    }

    const auto x_mm = ReadCoordinate(*pose_field, "x");
    const auto y_mm = ReadCoordinate(*pose_field, "y");
    const auto z_mm = ReadCoordinate(*pose_field, "z");
    if (!x_mm.has_value() || !y_mm.has_value() || !z_mm.has_value()) {
        return PoseResolution{ .ok = false,
                               .error_code = "ERR-GRIPPER-POSE-MALFORMED",
                               .message = "params.targetPose needs finite x, y and z in millimetres" };
    }

    const PickPose target{ .x_mm = *x_mm, .y_mm = *y_mm, .z_mm = *z_mm };

    /*
     * The claw would reach a target below the plate perfectly happily, so this
     * has to be checked before the solver rather than left to the joint limits.
     */
    if (target.z_mm < poses_.min_target_z_mm) {
        return PoseResolution{ .ok = false,
                               .error_code = "ERR-GRIPPER-POSE-BELOW-PLATE",
                               .message = "params.targetPose.z is below the configured floor" };
    }

    /*
     * Both waypoints are solved before either is sent.
     *
     * Solving the approach first and the descent later would let a cycle start,
     * lift the claw over the box, and only then discover that the descent itself
     * is unreachable -- aborting with the claw parked over the conveyor. Failing
     * the whole command up front keeps the arm where it is.
     */
    const IkSolution approach = SolveInverseKinematics(poses_.geometry, poses_.ApproachAbove(target));
    if (!approach.Succeeded()) {
        IkFailure failure = DescribeIkFailure(approach);
        return PoseResolution{ .ok = false,
                               .error_code = std::move(failure.error_code),
                               .message = "approach above pick pose: " + failure.message };
    }

    const IkSolution pick = SolveInverseKinematics(poses_.geometry, target);
    if (!pick.Succeeded()) {
        IkFailure failure = DescribeIkFailure(pick);
        return PoseResolution{ .ok = false,
                               .error_code = std::move(failure.error_code),
                               .message = "pick pose: " + failure.message };
    }

    return PoseResolution{ .ok = true,
                           .pick = ToGripperPose(pick.angles),
                           .approach = ToGripperPose(approach.angles),
                           .from_kinematics = true,
                           .error_code = {},
                           .message = {} };
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

GripperCommandResult GripperNode::StartCycle(const mqtt::ControlCommandPayload& command, GripperPhase phase) {
    GripperCommandResult result{ .status = GripperCommandStatus::kInvalidParameters,
                                 .mqtt_command = command.command,
                                 .request_id = command.request_id };

    const auto work_id = ReadStringParam(command.params, "workId");
    if (phase != GripperPhase::kHome && !work_id.has_value()) {
        EmitCommandResponse(result, "params.workId is required to execute a gripper cycle");
        return result;
    }
    result.work_id = work_id.value_or(std::string{});

    if (estop_latched_) {
        result.status = GripperCommandStatus::kRejected;
        EmitCommandResponse(result, "emergency stop is latched; send RECOVERY before starting a cycle");
        return result;
    }

    if (cycle_.active) {
        // The same work arriving twice is idempotent; a different work while one
        // is running would drop the box that is already in the claw.
        if (work_id.has_value() && *work_id == cycle_.work_id) {
            result.status = GripperCommandStatus::kDuplicate;
            EmitCommandResponse(result.request_id, command.command, mqtt::CommandResult::kDuplicated, std::nullopt,
                                "work is already being transferred");
            return result;
        }
        result.status = GripperCommandStatus::kActiveCycleConflict;
        EmitCommandResponse(result.request_id, command.command, mqtt::CommandResult::kRejected,
                            std::string("ERR-ACTIVE-CYCLE-CONFLICT"),
                            "another work is already being transferred: " + cycle_.work_id);
        return result;
    }

    // Motions are rejected by the controller until HOME has completed since boot
    // or since the last E-Stop, so refuse here with a reason the server can act on
    // instead of letting the first MOVE_ARM fail.
    if (!homed_ && phase != GripperPhase::kHome) {
        result.status = GripperCommandStatus::kNotHomed;
        EmitCommandResponse(result.request_id, command.command, mqtt::CommandResult::kRejected,
                            std::string("ERR-GRIPPER-NOT-HOMED"),
                            "arm is not homed; send INITIALIZE or RECOVERY with component_id=home first");
        return result;
    }

    /*
     * Resolved before the cycle is armed. A pose that cannot be reached is a
     * rejection of this command, not a cycle that starts and then aborts, so
     * nothing has moved and no cycle state needs unwinding.
     */
    PoseResolution resolved = ResolvePickPoses(command.params);
    if (!resolved.ok) {
        result.status = GripperCommandStatus::kUnreachablePose;
        EmitCommandResponse(result.request_id, command.command, mqtt::CommandResult::kRejected,
                            std::optional{ resolved.error_code }, resolved.message);
        return result;
    }

    cycle_ = ActiveCycle{
        .active = true,
        .mqtt_command = command.command,
        .phase = phase,
        .step = FirstStepOf(phase),
        .work_id = result.work_id,
        .request_id = command.request_id,
        .destination = ReadStringParam(command.params, "destination").value_or(std::string{}),
        .pick_pose = resolved.pick,
        .pick_approach_pose = resolved.approach,
        .from_kinematics = resolved.from_kinematics,
        .motion_id = UART_GRIPPER_MOTION_ID_NONE,
        .motion_type = 0U,
        .waited = {},
        .motion_budget = {},
    };

    if (!DispatchStep(result)) {
        // No response for this request has gone out yet, so unlike the
        // mid-cycle abort below, only one report belongs here: AbortCycle
        // would additionally publish its own (mislabeled) response for the
        // same request_id.
        const std::string message = "failed to start gripper cycle";
        cycle_ = ActiveCycle{};
        EmitCommandResponse(result, message);
        return result;
    }

    result.status = GripperCommandStatus::kAccepted;
    result.work_id = cycle_.work_id;
    EmitCommandResponse(result.request_id, command.command, mqtt::CommandResult::kProcessing, std::nullopt,
                        "gripper cycle accepted");
    EmitCycleStatus(std::string(ToString(cycle_.step)));
    return result;
}

GripperCommandResult GripperNode::RunStop(const mqtt::ControlCommandPayload& command) {
    GripperCommandResult result{ .status = GripperCommandStatus::kUartError,
                                 .mqtt_command = command.command,
                                 .request_id = command.request_id };

    result = Execute(std::move(result), UART_CMD_GRIPPER_STOP, {});
    if (result.Succeeded()) {
        running_requested_ = false;
        // STOP halts interpolation wherever it is, so the pose stops being known
        // whether or not a cycle was driving it.
        angles_known_ = false;
        if (cycle_.active) {
            AbortCycle("ERR-GRIPPER-STOPPED", "cycle stopped by operator command");
        }
        EmitCommandResponse(result, "gripper motion stopped");
        EmitDeviceStatus("STOPPED");
    } else {
        EmitCommandResponse(result, "gripper stop was not accepted by the controller");
    }
    return result;
}

GripperCommandResult GripperNode::RunInitialize(const mqtt::ControlCommandPayload& command) {
    GripperCommandResult result{ .status = GripperCommandStatus::kUartError,
                                 .mqtt_command = command.command,
                                 .request_id = command.request_id };

    // RESET clears a controller fault and the stale motion bookkeeping; the arm
    // is only actually safe to move again once HOME has completed after it.
    result = Execute(std::move(result), UART_CMD_GRIPPER_RESET, {});
    if (!result.Succeeded()) {
        EmitCommandResponse(result, "gripper reset was not accepted by the controller");
        return result;
    }

    if (cycle_.active) {
        AbortCycle("ERR-GRIPPER-RESET", "cycle cancelled by initialize");
    }
    homed_ = false;
    angles_known_ = false;

    cycle_ = ActiveCycle{
        .active = true,
        .mqtt_command = command.command,
        .phase = GripperPhase::kHome,
        .step = GripperCycleStep::kReturnHome,
        .work_id = {},
        .request_id = command.request_id,
        .destination = {},
        // Homing never visits either pick waypoint; the taught values are carried
        // only so the struct has no half-initialised members.
        .pick_pose = poses_.pick,
        .pick_approach_pose = poses_.pick_approach,
        .from_kinematics = false,
        .motion_id = UART_GRIPPER_MOTION_ID_NONE,
        .motion_type = 0U,
        .waited = {},
        .motion_budget = {},
    };

    if (!DispatchStep(result)) {
        // Same reasoning as StartCycle: this request has not been answered yet,
        // so AbortCycle's own (kStart-labeled) response would duplicate this one.
        const std::string message = "gripper reset succeeded but homing could not be started";
        cycle_ = ActiveCycle{};
        EmitCommandResponse(result, message);
        return result;
    }

    result.status = GripperCommandStatus::kAccepted;
    EmitCommandResponse(result.request_id, command.command, mqtt::CommandResult::kProcessing, std::nullopt,
                        "gripper reset accepted; homing in progress");
    EmitDeviceStatus("HOMING");
    return result;
}

GripperCommandResult GripperNode::RunRecovery(const mqtt::ControlCommandPayload& command) {
    GripperCommandResult result{ .status = GripperCommandStatus::kUartError,
                                 .mqtt_command = command.command,
                                 .request_id = command.request_id };

    result = ExecuteAsync(std::move(result), UART_CMD_RESET_DEVICE, {});
    if (result.Succeeded()) {
        pending_safety_ = PendingSafetyCommand{ .active = true,
                                                .expected = PendingSafetyEvent::kReleased,
                                                .command = command.command,
                                                .request_id = command.request_id,
                                                .elapsed = {} };
    } else {
        EmitCommandResponse(result, "safety reset could not be written to the controller");
    }
    return result;
}

GripperCommandResult GripperNode::RunStatusRequest(const mqtt::ControlCommandPayload& command) {
    GripperCommandResult result{ .status = GripperCommandStatus::kUartError,
                                 .mqtt_command = command.command,
                                 .request_id = command.request_id };

    result = Execute(std::move(result), UART_CMD_GRIPPER_GET_STATUS, {});
    if (result.Succeeded()) {
        EmitControllerStatus(result.uart_result.response_frame);
        EmitCommandResponse(result, "gripper status reported");
    } else {
        EmitCommandResponse(result, "gripper status request failed");
    }
    return result;
}

// ---------------------------------------------------------------------------
// Sequencer
// ---------------------------------------------------------------------------

GripperCycleStep GripperNode::FirstStepOf(GripperPhase phase) noexcept {
    switch (phase) {
        case GripperPhase::kFullCycle:
        case GripperPhase::kPick:
            return GripperCycleStep::kOpenClaw;
        case GripperPhase::kTransfer:
            return GripperCycleStep::kPickRetreat;
        case GripperPhase::kPlace:
            return GripperCycleStep::kPlaceDescend;
        case GripperPhase::kHome:
            return GripperCycleStep::kReturnHome;
    }
    return GripperCycleStep::kIdle;
}

GripperCycleStep GripperNode::NextStep(GripperPhase phase, GripperCycleStep step) noexcept {
    switch (step) {
        case GripperCycleStep::kOpenClaw:
            return GripperCycleStep::kPickApproach;
        case GripperCycleStep::kPickApproach:
            return GripperCycleStep::kPickDescend;
        case GripperCycleStep::kPickDescend:
            return GripperCycleStep::kCloseClaw;
        case GripperCycleStep::kCloseClaw:
            // "Pick only" stops with the box held above the input conveyor so a
            // separate TRANSFER command can continue from there.
            return (phase == GripperPhase::kPick) ? GripperCycleStep::kCompleted : GripperCycleStep::kPickRetreat;
        case GripperCycleStep::kPickRetreat:
            return GripperCycleStep::kTransfer;
        case GripperCycleStep::kTransfer:
            return GripperCycleStep::kPlaceApproach;
        case GripperCycleStep::kPlaceApproach:
            return (phase == GripperPhase::kTransfer) ? GripperCycleStep::kCompleted : GripperCycleStep::kPlaceDescend;
        case GripperCycleStep::kPlaceDescend:
            return GripperCycleStep::kReleaseClaw;
        case GripperCycleStep::kReleaseClaw:
            return GripperCycleStep::kPlaceRetreat;
        case GripperCycleStep::kPlaceRetreat:
            return (phase == GripperPhase::kPlace) ? GripperCycleStep::kCompleted : GripperCycleStep::kReturnTransfer;
        case GripperCycleStep::kReturnTransfer:
            return GripperCycleStep::kReturnPickSide;
        case GripperCycleStep::kReturnPickSide:
            return GripperCycleStep::kReturnHome;
        case GripperCycleStep::kReturnHome:
            return GripperCycleStep::kCompleted;
        case GripperCycleStep::kIdle:
        case GripperCycleStep::kCompleted:
            return GripperCycleStep::kCompleted;
    }
    return GripperCycleStep::kCompleted;
}

bool GripperNode::DispatchStep(GripperCommandResult& result) {
    const std::uint16_t motion_id = AllocateMotionId();
    std::uint8_t uart_command{};
    std::uint8_t motion_type{};
    /*
     * Two durations, and they are not the same number.
     *
     * The controller runs the motion for max(what we sent, what its own speed
     * limits require) without telling us, so the value on the wire is what we
     * asked for while expected_duration_ms is what will actually happen. Timing
     * out against the wire value is what made correct motions look like lost
     * completion events.
     */
    std::uint32_t expected_duration_ms{};

    const auto send_arm = [&](const GripperPose& pose) {
        const JointAngles target = ToJointAngles(pose);
        /*
         * Without a trustworthy current position the travel cannot be predicted,
         * so assume the longest motion the joint limits allow. That is the
         * contract ceiling: the shoulder's 1200 deci-degree span at 120
         * deci-degrees per second is exactly 10 s.
         */
        const std::uint32_t minimum_ms = angles_known_
                                             ? MinimumArmDurationMs(poses_.geometry, commanded_angles_, target)
                                             : static_cast<std::uint32_t>(UART_GRIPPER_DURATION_MS_MAX);
        expected_duration_ms = std::max<std::uint32_t>(poses_.arm_duration_ms, minimum_ms);
        const std::uint16_t duration_ms = ClampDurationMs(expected_duration_ms);

        std::array<std::uint8_t, UART_GRIPPER_MOVE_ARM_PAYLOAD_SIZE> payload{};
        WriteU16(payload.data(), UART_GRIPPER_MOVE_MOTION_ID_LOW_INDEX, motion_id);
        WriteU16(payload.data(), UART_GRIPPER_MOVE_BASE_ANGLE_LOW_INDEX, pose.base_deci_deg);
        WriteU16(payload.data(), UART_GRIPPER_MOVE_SHOULDER_ANGLE_LOW_INDEX, pose.shoulder_deci_deg);
        WriteU16(payload.data(), UART_GRIPPER_MOVE_ELBOW_ANGLE_LOW_INDEX, pose.elbow_deci_deg);
        WriteU16(payload.data(), UART_GRIPPER_MOVE_DURATION_LOW_INDEX, duration_ms);
        uart_command = UART_CMD_GRIPPER_MOVE_ARM;
        motion_type = UART_GRIPPER_MOTION_ARM;
        result = Execute(std::move(result), uart_command, payload);
        if (result.Succeeded()) {
            commanded_angles_ = target;
        }
    };

    const auto send_claw = [&](std::uint8_t position_percent) {
        const std::uint32_t minimum_ms =
            angles_known_ ? MinimumClawDurationMs(poses_.geometry, commanded_claw_percent_, position_percent)
                          : static_cast<std::uint32_t>(UART_GRIPPER_DURATION_MS_MAX);
        expected_duration_ms = std::max<std::uint32_t>(poses_.claw_duration_ms, minimum_ms);
        const std::uint16_t duration_ms = ClampDurationMs(expected_duration_ms);

        std::array<std::uint8_t, UART_GRIPPER_SET_GRIPPER_PAYLOAD_SIZE> payload{};
        WriteU16(payload.data(), UART_GRIPPER_SET_MOTION_ID_LOW_INDEX, motion_id);
        payload[UART_GRIPPER_SET_POSITION_INDEX] = position_percent;
        WriteU16(payload.data(), UART_GRIPPER_SET_DURATION_LOW_INDEX, duration_ms);
        uart_command = UART_CMD_GRIPPER_SET_GRIPPER;
        motion_type = UART_GRIPPER_MOTION_GRIPPER;
        result = Execute(std::move(result), uart_command, payload);
        if (result.Succeeded()) {
            commanded_claw_percent_ = position_percent;
        }
    };

    switch (cycle_.step) {
        case GripperCycleStep::kOpenClaw:
        case GripperCycleStep::kReleaseClaw:
            send_claw(poses_.open_position_percent);
            break;

        case GripperCycleStep::kCloseClaw:
            send_claw(poses_.closed_position_percent);
            break;

        case GripperCycleStep::kPickApproach:
            send_arm(cycle_.pick_approach_pose);
            break;

        case GripperCycleStep::kPickRetreat:
        case GripperCycleStep::kReturnPickSide:
            send_arm(poses_.pick_lift);
            break;

        case GripperCycleStep::kTransfer:
        case GripperCycleStep::kReturnTransfer:
            send_arm(poses_.transfer);
            break;

        case GripperCycleStep::kPickDescend:
            send_arm(cycle_.pick_pose);
            break;

        case GripperCycleStep::kPlaceApproach:
        case GripperCycleStep::kPlaceRetreat:
            send_arm(poses_.place_approach);
            break;

        case GripperCycleStep::kPlaceDescend:
            send_arm(poses_.place);
            break;

        case GripperCycleStep::kReturnHome: {
            std::array<std::uint8_t, UART_GRIPPER_HOME_PAYLOAD_SIZE> payload{};
            WriteU16(payload.data(), UART_GRIPPER_HOME_MOTION_ID_LOW_INDEX, motion_id);
            uart_command = UART_CMD_GRIPPER_HOME;
            motion_type = UART_GRIPPER_MOTION_HOME;
            /*
             * HOME carries no duration of its own: the controller picks
             * GRIPPER_HOME_DURATION_MS and then stretches it for both the arm and
             * the claw, whose travel we may not know. The full ceiling is the only
             * budget that cannot cut a legitimate homing short, and homing is rare
             * enough that waiting longer on a genuinely lost event costs little.
             */
            expected_duration_ms = static_cast<std::uint32_t>(UART_GRIPPER_DURATION_MS_MAX);
            result = Execute(std::move(result), uart_command, payload);
            break;
        }

        case GripperCycleStep::kIdle:
        case GripperCycleStep::kCompleted:
            return false;
    }

    if (!result.Succeeded()) {
        return false;
    }

    cycle_.motion_id = motion_id;
    cycle_.motion_type = motion_type;
    cycle_.waited = std::chrono::milliseconds{ 0 };
    cycle_.motion_budget = std::chrono::milliseconds{ expected_duration_ms } + kMotionTimeoutSlack;
    result.motion_id = motion_id;
    return true;
}

void GripperNode::AdvanceCycle() {
    const GripperCycleStep previous = cycle_.step;
    cycle_.step = NextStep(cycle_.phase, cycle_.step);

    if (cycle_.step == GripperCycleStep::kCompleted) {
        if (previous == GripperCycleStep::kReturnHome) {
            homed_ = true;
            // A completed HOME is the one moment the controller's pose is known
            // exactly, so it re-anchors the duration arithmetic.
            commanded_angles_ = poses_.geometry.HomeAngles();
            commanded_claw_percent_ = poses_.geometry.home_claw_percent;
            angles_known_ = true;
        }
        FinishCycle();
        return;
    }

    GripperCommandResult dispatch{ .status = GripperCommandStatus::kUartError,
                                   .mqtt_command = cycle_.mqtt_command,
                                   .request_id = cycle_.request_id,
                                   .work_id = cycle_.work_id };
    if (!DispatchStep(dispatch)) {
        AbortCycle("ERR-GRIPPER-DISPATCH", "next gripper motion could not be sent");
        return;
    }

    const std::string state{ ToString(cycle_.step) };
    if (state != ToString(previous)) {
        EmitCycleStatus(state);
    }
}

void GripperNode::FinishCycle() {
    const std::string work_id = cycle_.work_id;
    const std::string request_id = cycle_.request_id;
    const mqtt::ControlCommand mqtt_command = cycle_.mqtt_command;
    const bool was_home_only = cycle_.phase == GripperPhase::kHome;

    cycle_ = ActiveCycle{};

    if (was_home_only) {
        EmitCommandResponse(request_id, mqtt_command, mqtt::CommandResult::kSuccess, std::nullopt,
                            "gripper returned home");
        const bool enters_running = mqtt_command == mqtt::ControlCommand::kStart ||
                                    mqtt_command == mqtt::ControlCommand::kRestart;
        EmitDeviceStatus(enters_running ? "RUNNING" : "READY");
        return;
    }

    EmitCommandResponse(request_id, mqtt_command, mqtt::CommandResult::kSuccess, std::nullopt,
                        "gripper transfer completed");
    // COMPLETED with the job ID attached is what advances the server's work state
    // machine, so it is published exactly once and only from here.
    EmitDeviceStatus("COMPLETED", std::nullopt, work_id.empty() ? std::nullopt : std::optional{ work_id });
}

void GripperNode::AbortCycle(std::string error_code, std::string message) {
    if (!cycle_.active) {
        return;
    }
    /*
     * Every abort leaves the arm part-way through an interpolation, at a pose
     * neither side can name. Keeping the last commanded target would make the
     * next move's predicted duration too short and time it out, so tracking is
     * dropped until a HOME or GET_STATUS re-establishes it.
     */
    angles_known_ = false;
    const std::string work_id = cycle_.work_id;
    const std::string request_id = cycle_.request_id;
    const mqtt::ControlCommand mqtt_command = cycle_.mqtt_command;
    cycle_ = ActiveCycle{};

    const std::optional<std::string> job_id = work_id.empty() ? std::nullopt : std::optional{ work_id };
    EmitCommandResponse(request_id, mqtt_command, mqtt::CommandResult::kFailed, std::optional{ error_code }, message);
    EmitError(std::move(error_code), "ERROR", std::move(message), job_id);
}

// ---------------------------------------------------------------------------
// UART execution
// ---------------------------------------------------------------------------

GripperCommandResult GripperNode::Execute(GripperCommandResult result, std::uint8_t command,
                                          std::span<const std::uint8_t> payload) {
    result.uart_command = command;
    if (!uart_session_.IsOpen()) {
        result.status = GripperCommandStatus::kUartNotOpen;
        return result;
    }

    result.uart_result = uart_session_.Transact(command, payload);
    switch (result.uart_result.status) {
        case InputTransactStatus::kSuccess:
            result.status = GripperCommandStatus::kSuccess;
            break;
        case InputTransactStatus::kAccepted:
            // ACK: the controller took the motion and will report completion
            // later via an EVENT. DispatchStep's caller only checks
            // Succeeded(), which is true for both this and kSuccess, but this
            // keeps the "already finished" vs "started" distinction visible on
            // the GripperCommandResult itself.
            result.status = GripperCommandStatus::kAccepted;
            break;
        case InputTransactStatus::kRejected:
            result.status = (result.uart_result.response_error == UART_GRIPPER_ERROR_NOT_HOMED)
                                ? GripperCommandStatus::kNotHomed
                                : GripperCommandStatus::kRejected;
            break;
        case InputTransactStatus::kTimeout:
            result.status = GripperCommandStatus::kTimeout;
            break;
        case InputTransactStatus::kNotOpen:
            result.status = GripperCommandStatus::kUartNotOpen;
            break;
        case InputTransactStatus::kInvalidArgument:
            result.status = GripperCommandStatus::kInvalidParameters;
            break;
        case InputTransactStatus::kSent:
        case InputTransactStatus::kEncodeError:
        case InputTransactStatus::kTransportError:
            result.status = GripperCommandStatus::kUartError;
            break;
    }
    return result;
}

GripperCommandResult GripperNode::ExecuteAsync(GripperCommandResult result, std::uint8_t command,
                                               std::span<const std::uint8_t> payload) {
    result.uart_command = command;
    if (!uart_session_.IsOpen()) {
        result.status = GripperCommandStatus::kUartNotOpen;
        return result;
    }

    result.uart_result = uart_session_.SendCommand(command, payload);
    result.status = (result.uart_result.status == InputTransactStatus::kSent) ? GripperCommandStatus::kAccepted
                                                                              : GripperCommandStatus::kUartError;
    return result;
}

// ---------------------------------------------------------------------------
// Controller frames
// ---------------------------------------------------------------------------

void GripperNode::HandleUartFrame(const uart_frame_t& frame) {
    switch (frame.command) {
        case UART_CMD_EVENT:
            HandleControllerEvent(frame);
            break;
        case UART_CMD_DEVICE_STATUS:
            HandleDeviceStatus(frame);
            break;
        default:
            break;
    }
}

void GripperNode::HandleControllerEvent(const uart_frame_t& frame) {
    if (frame.length < UART_EVENT_HEADER_SIZE) {
        return;
    }
    const std::uint8_t event_id = frame.payload[UART_EVENT_ID_INDEX];

    // Both families share these IDs; the length decides which one this is.
    if (event_id == kAppEventHeartbeat) {
        if (frame.length == kHeartbeatPayloadSize) {
            HandleControllerHeartbeat(frame);
        } else if (frame.length == UART_GRIPPER_MOTION_EVENT_PAYLOAD_SIZE) {
            HandleMotionComplete(frame);
        }
        return;
    }

    if (event_id == kAppEventSafety) {
        if (frame.length == kSafetyEventPayloadSize) {
            HandleSafetyEvent(frame);
        } else if (frame.length == UART_GRIPPER_FAULT_EVENT_PAYLOAD_SIZE) {
            HandleMotionFault(frame);
        }
        return;
    }
}

void GripperNode::HandleMotionComplete(const uart_frame_t& frame) {
    const std::uint16_t motion_id = ReadU16(frame.payload, UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX);
    const std::uint8_t motion_type = frame.payload[UART_GRIPPER_EVENT_MOTION_TYPE_INDEX];

    // A completion for a motion that is no longer current belongs to a cycle that
    // was stopped or superseded; acting on it would advance the wrong sequence.
    if (!cycle_.active || motion_id != cycle_.motion_id || motion_type != cycle_.motion_type) {
        return;
    }
    AdvanceCycle();
}

void GripperNode::HandleMotionFault(const uart_frame_t& frame) {
    const std::uint16_t motion_id = ReadU16(frame.payload, UART_GRIPPER_EVENT_MOTION_ID_LOW_INDEX);
    const std::uint8_t error = frame.payload[UART_GRIPPER_FAULT_EVENT_ERROR_INDEX];

    if (error == UART_ERROR_EMERGENCY_STOP) {
        homed_ = false;
    }
    // A fault stops the servos part-way regardless of whether the faulted motion
    // belonged to the current cycle, so AbortCycle below is not enough on its own.
    angles_known_ = false;
    if (!cycle_.active || motion_id != cycle_.motion_id) {
        EmitError(DescribeUartError(error), "ERROR", "gripper reported a motion fault", std::nullopt);
        return;
    }
    AbortCycle(DescribeUartError(error), "gripper motion faulted during " + std::string(ToString(cycle_.step)));
}

void GripperNode::HandleSafetyEvent(const uart_frame_t& frame) {
    const bool latched = frame.payload[kSafetyEventLatchedIndex] != 0U;
    const std::uint8_t cause = frame.payload[kSafetyEventCauseIndex];
    const bool requested = estop_requested_ ||
        pending_safety_.active &&
        ((pending_safety_.expected == PendingSafetyEvent::kEstopLatched && latched) ||
         (pending_safety_.expected == PendingSafetyEvent::kReleased && !latched));
    estop_latched_ = latched;

    if (latched) {
        running_requested_ = false;
        // The controller drops the active motion and forgets its home reference,
        // so both must be invalidated here to keep this node's view honest.
        homed_ = false;
        angles_known_ = false;
        if (cycle_.active) {
            AbortCycle("ERR-EMERGENCY-STOP", "cycle aborted by emergency stop");
        }
    }

    if (requested) {
        EmitCommandResponse(
            pending_safety_.request_id, pending_safety_.command, mqtt::CommandResult::kSuccess, std::nullopt,
            latched ? "emergency stop latched" : "emergency stop released; send RECOVERY to home the arm");
        pending_safety_ = PendingSafetyCommand{};
    }

    if (latched) {
        if (!requested) {
            EmitError("ERR-EMERGENCY-STOP", "CRITICAL",
                      "controller latched an emergency stop (cause " + HexByte(cause) + ")", std::nullopt);
        }
        EmitDeviceStatus("EMERGENCY_STOP",
                         requested ? std::nullopt : std::optional<std::string>{ "ERR-EMERGENCY-STOP" });
    } else {
        // Released but deliberately not homed: report STOPPED, never READY, so the
        // server does not treat the arm as ready to accept work yet.
        estop_requested_ = false;
        EmitDeviceStatus("STOPPED");
    }
}

void GripperNode::HandleControllerHeartbeat(const uart_frame_t& frame) {
    since_controller_heartbeat_ = std::chrono::milliseconds{ 0 };
    const HeartbeatState state{ .device_state = frame.payload[kHeartbeatStateIndex],
                                .error_code = frame.payload[kHeartbeatErrorIndex] };

    if (controller_heartbeat_lost_) {
        controller_heartbeat_lost_ = false;
        EmitError("ERR-CONTROLLER-HEARTBEAT-RECOVERED", "WARNING", "controller heartbeat resumed", std::nullopt);
    }

    // Only state changes are published; a 1 Hz heartbeat would otherwise flood the
    // status topic with identical payloads.
    if (last_heartbeat_state_.has_value() && last_heartbeat_state_->device_state == state.device_state &&
        last_heartbeat_state_->error_code == state.error_code) {
        return;
    }
    last_heartbeat_state_ = state;

    // While a cycle runs, the sequencer owns the reported state; overwriting it
    // with the controller's coarse RUNNING/READY would either mask progress or,
    // worse, publish READY and make the server think the transfer finished.
    if (cycle_.active) {
        return;
    }

    const std::optional<std::string> error_code =
        (state.error_code == UART_ERROR_NONE ||
         (estop_requested_ && state.error_code == UART_ERROR_EMERGENCY_STOP))
            ? std::nullopt
            : std::optional{ DescribeUartError(state.error_code) };
    EmitDeviceStatus(running_requested_ && state.device_state == UART_DEVICE_READY ? "RUNNING"
                                                                                   : DescribeDeviceState(state.device_state),
                     error_code);
}

void GripperNode::HandleDeviceStatus(const uart_frame_t& frame) {
    if (frame.length < 2U) {
        return;
    }
    const std::uint8_t device_state = frame.payload[0];
    const std::uint8_t error_code = frame.payload[1];
    if (cycle_.active) {
        return;
    }
    const std::optional<std::string> error =
        (error_code == UART_ERROR_NONE || (estop_requested_ && error_code == UART_ERROR_EMERGENCY_STOP))
            ? std::nullopt
            : std::optional{ DescribeUartError(error_code) };
    EmitDeviceStatus(running_requested_ && device_state == UART_DEVICE_READY ? "RUNNING"
                                                                              : DescribeDeviceState(device_state),
                     error);
}

void GripperNode::EmitControllerStatus(const uart_frame_t& response) {
    if (response.length < UART_GRIPPER_STATUS_PAYLOAD_SIZE) {
        return;
    }
    const std::uint8_t state = response.payload[UART_GRIPPER_STATUS_STATE_INDEX];
    homed_ = response.payload[UART_GRIPPER_STATUS_HOMED_INDEX] != 0U;
    estop_latched_ = state == UART_GRIPPER_STATE_EMERGENCY_STOP;

    /*
     * GET_STATUS is the only frame that reports where the arm actually is, so it
     * re-anchors the duration arithmetic after a stop or fault dropped it. The
     * angles are only taken while the arm is still, because mid-interpolation they
     * describe a pose the arm has already left.
     */
    if (state == UART_GRIPPER_STATE_IDLE || state == UART_GRIPPER_STATE_STOPPED) {
        commanded_angles_ = JointAngles{ ReadU16(response.payload, UART_GRIPPER_STATUS_BASE_ANGLE_LOW_INDEX),
                                         ReadU16(response.payload, UART_GRIPPER_STATUS_SHOULDER_ANGLE_LOW_INDEX),
                                         ReadU16(response.payload, UART_GRIPPER_STATUS_ELBOW_ANGLE_LOW_INDEX) };
        commanded_claw_percent_ = response.payload[UART_GRIPPER_STATUS_POSITION_INDEX];
        angles_known_ = true;
    }

    const std::optional<std::string> error_code =
        (estop_latched_ && !estop_requested_) ? std::optional{ std::string("ERR-EMERGENCY-STOP") } : std::nullopt;
    EmitDeviceStatus(running_requested_ && state == UART_GRIPPER_STATE_IDLE ? "RUNNING" : DescribeGripperState(state),
                     error_code);
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

void GripperNode::Tick(std::chrono::milliseconds elapsed) {
    if (cycle_.active && cycle_.motion_id != UART_GRIPPER_MOTION_ID_NONE) {
        cycle_.waited += elapsed;
        if (cycle_.waited >= cycle_.motion_budget) {
            AbortCycle("ERR-GRIPPER-MOTION-TIMEOUT", "no completion event for the active gripper motion");
        }
    }

    if (pending_safety_.active) {
        pending_safety_.elapsed += elapsed;
        if (pending_safety_.elapsed >= kSafetyEventTimeout) {
            EmitCommandResponse(pending_safety_.request_id, pending_safety_.command, mqtt::CommandResult::kTimeout,
                                std::string("ERR-SAFETY-EVENT-TIMEOUT"),
                                "controller did not confirm the safety change");
            if (pending_safety_.expected == PendingSafetyEvent::kEstopLatched) {
                estop_requested_ = false;
            }
            pending_safety_ = PendingSafetyCommand{};
        }
    }

    since_controller_heartbeat_ += elapsed;
    if (!controller_heartbeat_lost_ && since_controller_heartbeat_ >= kControllerHeartbeatTimeout) {
        controller_heartbeat_lost_ = true;
        EmitError("ERR-CONTROLLER-HEARTBEAT-LOST", "ERROR", "no controller heartbeat within the expected interval",
                  std::nullopt);
    }
}

void GripperNode::ResetControllerHeartbeatMonitor() noexcept {
    since_controller_heartbeat_ = std::chrono::milliseconds{ 0 };
    controller_heartbeat_lost_ = false;
    last_heartbeat_state_.reset();
    // A reconnect says nothing about the arm's pose, so the home reference and the
    // tracked joint angles are both dropped until GET_STATUS or a completed HOME
    // proves otherwise.
    homed_ = false;
    angles_known_ = false;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void GripperNode::EmitCommandResponse(const GripperCommandResult& result, std::string message) const {
    mqtt::CommandResult outcome = mqtt::CommandResult::kFailed;
    std::optional<std::string> error_code;

    switch (result.status) {
        case GripperCommandStatus::kSuccess:
            outcome = mqtt::CommandResult::kSuccess;
            break;
        case GripperCommandStatus::kAccepted:
            outcome = mqtt::CommandResult::kProcessing;
            break;
        case GripperCommandStatus::kDuplicate:
            outcome = mqtt::CommandResult::kDuplicated;
            break;
        case GripperCommandStatus::kTimeout:
            outcome = mqtt::CommandResult::kTimeout;
            error_code = "ERR-CONTROLLER-TIMEOUT";
            break;
        case GripperCommandStatus::kNotHomed:
            outcome = mqtt::CommandResult::kRejected;
            error_code = "ERR-GRIPPER-NOT-HOMED";
            break;
        case GripperCommandStatus::kActiveCycleConflict:
            outcome = mqtt::CommandResult::kRejected;
            error_code = "ERR-ACTIVE-CYCLE-CONFLICT";
            break;
        case GripperCommandStatus::kRejected:
            outcome = mqtt::CommandResult::kRejected;
            error_code = DescribeUartError(result.uart_result.response_error);
            break;
        case GripperCommandStatus::kUnsupportedMessage:
        case GripperCommandStatus::kUnsupportedCommand:
            outcome = mqtt::CommandResult::kRejected;
            error_code = "ERR-UNSUPPORTED-COMMAND";
            break;
        case GripperCommandStatus::kInvalidMessage:
        case GripperCommandStatus::kInvalidParameters:
            outcome = mqtt::CommandResult::kRejected;
            error_code = "ERR-INVALID-COMMAND";
            break;
        case GripperCommandStatus::kInvalidTarget:
            outcome = mqtt::CommandResult::kRejected;
            error_code = "ERR-INVALID-TARGET";
            break;
        case GripperCommandStatus::kUnreachablePose:
            /*
             * StartCycle answers an unreachable pose itself so it can name the
             * joint or the reach boundary that blocked it. Reaching here means
             * some other path produced the status without that detail, so the
             * generic code is the honest answer rather than a guess.
             */
            outcome = mqtt::CommandResult::kRejected;
            error_code = "ERR-GRIPPER-UNREACHABLE-POSE";
            break;
        case GripperCommandStatus::kUartNotOpen:
            outcome = mqtt::CommandResult::kFailed;
            error_code = "ERR-UART-DISCONNECTED";
            break;
        case GripperCommandStatus::kControllerFailure:
        case GripperCommandStatus::kUartError:
            outcome = mqtt::CommandResult::kFailed;
            error_code = "ERR-UART-IO";
            break;
    }

    EmitCommandResponse(result.request_id, result.mqtt_command, outcome, std::move(error_code), std::move(message));
}

void GripperNode::EmitCommandResponse(std::string request_id, mqtt::ControlCommand command, mqtt::CommandResult result,
                                      std::optional<std::string> error_code, std::string message) const {
    if (request_id.empty()) {
        return;
    }
    EmitReport(GripperReport{ .channel = GripperReportChannel::kResponse,
                              .message_type = mqtt::MessageType::kCommandResponse,
                              .data = mqtt::CommandResponsePayload{ .request_id = std::move(request_id),
                                                                    .command = command,
                                                                    .result = result,
                                                                    .error_code = std::move(error_code),
                                                                    .message = std::move(message) } });
}

void GripperNode::EmitCycleStatus(std::string current_state) const {
    EmitDeviceStatus(std::move(current_state), std::nullopt,
                     cycle_.work_id.empty() ? std::nullopt : std::optional{ cycle_.work_id });
}

void GripperNode::EmitDeviceStatus(std::string current_state, std::optional<std::string> error_code,
                                   std::optional<std::string> job_id) const {
    EmitReport(GripperReport{ .channel = GripperReportChannel::kStatus,
                              .message_type = mqtt::MessageType::kDeviceStatus,
                              .data = mqtt::DeviceStatusPayload{ .status = error_code.has_value()
                                                                               ? mqtt::ConnectionState::kUartError
                                                                               : mqtt::ConnectionState::kOnline,
                                                                 .current_state = std::move(current_state),
                                                                 .job_id = std::move(job_id),
                                                                 .error_code = std::move(error_code) } });
}

void GripperNode::EmitError(std::string error_code, std::string error_level, std::string message,
                            std::optional<std::string> job_id) const {
    EmitReport(GripperReport{ .channel = GripperReportChannel::kError,
                              .message_type = mqtt::MessageType::kErrorOccurred,
                              .data = mqtt::ErrorOccurredPayload{ .job_id = std::move(job_id),
                                                                  .error_code = std::move(error_code),
                                                                  .error_level = std::move(error_level),
                                                                  .current_state = "GRIPPER",
                                                                  .message = std::move(message),
                                                                  .distance = std::nullopt } });
}

void GripperNode::EmitReport(GripperReport report) const {
    if (report_handler_) {
        report_handler_(report);
    }
}

}  // namespace logistics::device
