#include "logistics/device/node_runtime.hpp"

#ifdef LOGISTICS_GRIPPER_DAEMON_ENABLED

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/device/device_status.hpp"
#include "logistics/device/gripper_node.hpp"
#include "logistics/device/gripper_pose_config.hpp"
#include "logistics/device/input_uart_session.hpp"
#include "logistics/device/mqtt_node_client.hpp"
#include "logistics/device/mqtt_node_config.hpp"
#include "logistics/device/mqtt_time.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;
using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kCommandQueueCapacity = 64U;
inline constexpr std::size_t kOutboundQueueCapacity = 256U;
inline constexpr auto kSpontaneousPollTimeout = std::chrono::milliseconds{ 20 };
inline constexpr auto kUartReconnectInterval = std::chrono::seconds{ 2 };
inline constexpr auto kIdleDelay = std::chrono::milliseconds{ 5 };

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) {
    stop_requested = 1;
}

[[nodiscard]] std::filesystem::path ResolveConfigPath(int argc, char* argv[]) {
    if (argc > 1 && argv[1] != nullptr && std::string_view(argv[1]).size() != 0U) {
        return argv[1];
    }
    if (const char* environment_path = std::getenv("LOGISTICS_DEVICE_CONFIG");
        environment_path != nullptr && *environment_path != '\0') {
        return environment_path;
    }
    return std::filesystem::path("device-rpi") / "config" / "node.ini";
}

[[nodiscard]] std::string ResolveUartPath(int argc, char* argv[]) {
    if (argc > 2 && argv[2] != nullptr && std::string_view(argv[2]).size() != 0U) {
        return argv[2];
    }
    if (const char* environment_path = std::getenv("LOGISTICS_UART_DEVICE");
        environment_path != nullptr && *environment_path != '\0') {
        return environment_path;
    }
    return std::string(UartTransport::kDefaultDevicePath);
}

class CommandInbox final {
public:
    [[nodiscard]] bool Push(const mqtt::MqttMessage& message) {
        std::lock_guard lock(mutex_);
        if (messages_.size() >= kCommandQueueCapacity) {
            return false;
        }
        messages_.push_back(message);
        return true;
    }

    [[nodiscard]] std::deque<mqtt::MqttMessage> TakeAll() {
        std::deque<mqtt::MqttMessage> messages;
        std::lock_guard lock(mutex_);
        messages.swap(messages_);
        return messages;
    }

private:
    std::mutex mutex_;
    std::deque<mqtt::MqttMessage> messages_;
};

struct OutboundMessage {
    GripperReportChannel channel{ GripperReportChannel::kStatus };
    mqtt::MqttMessage message;
};

[[nodiscard]] OutboundMessage MakeOutboundMessage(const GripperReport& report, std::string_view device_id,
                                                  std::string_view message_session_id,
                                                  std::uint64_t& message_sequence) {
    return {
        .channel = report.channel,
        .message =
            mqtt::MqttMessage{
                .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
                .message_id = MakeMessageId(device_id, message_session_id, message_sequence++),
                .message_type = report.message_type,
                .source_id = std::string(device_id),
                .timestamp = CurrentIso8601Timestamp(),
                .data = report.data,
            },
    };
}

void UpdateDeviceStatus(const GripperReport& report, DeviceStatus& device_status) {
    if (const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&report.data); status != nullptr) {
        // The heartbeat carries this state, which is what makes the gripper's own
        // device ID and live state visible to the server between work cycles.
        device_status.SetCurrentState(status->current_state);
        device_status.SetJobId(status->job_id);
        device_status.SetErrorCode(status->error_code);
        return;
    }
    if (const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&report.data); error != nullptr) {
        // An asynchronous fault updates the active error code but must not
        // overwrite the operational state, or a transient event would mask the
        // real arm state until the next status report.
        device_status.SetErrorCode(error->error_code);
    }
}

[[nodiscard]] bool PublishOutbound(MqttNodeClient& mqtt_client, const OutboundMessage& outbound);

[[nodiscard]] bool EnqueueOutbound(std::deque<OutboundMessage>& outbox, const GripperReport& report,
                                   std::string_view device_id, std::string_view message_session_id,
                                   std::uint64_t& message_sequence, DeviceStatus& device_status,
                                   MqttNodeClient* durable_client = nullptr) {
    UpdateDeviceStatus(report, device_status);
    auto outbound = MakeOutboundMessage(report, device_id, message_session_id, message_sequence);
    if (durable_client != nullptr && report.channel != GripperReportChannel::kStatus &&
        PublishOutbound(*durable_client, outbound)) {
        return true;
    }
    if (outbox.size() >= kOutboundQueueCapacity) {
        // Status snapshots are the only safely droppable messages; command
        // responses and cycle events must survive a broker outage.
        const auto stale_status = std::find_if(outbox.begin(), outbox.end(), [](const OutboundMessage& queued) {
            return queued.channel == GripperReportChannel::kStatus;
        });
        if (stale_status == outbox.end()) {
            std::cerr << "[gripper][mqtt][ERROR] outbound queue full; preserving queued responses and events\n";
            return false;
        }
        outbox.erase(stale_status);
    }
    outbox.push_back(std::move(outbound));
    return true;
}

[[nodiscard]] bool PublishOutbound(MqttNodeClient& mqtt_client, const OutboundMessage& outbound) {
    switch (outbound.channel) {
        case GripperReportChannel::kResponse:
            return mqtt_client.PublishResponse(outbound.message);
        case GripperReportChannel::kStatus:
            return mqtt_client.PublishStatus(outbound.message);
        case GripperReportChannel::kEvent:
            return mqtt_client.PublishEvent(outbound.message);
        case GripperReportChannel::kError:
            return mqtt_client.PublishError(outbound.message);
    }
    return false;
}

void FlushOutbox(MqttNodeClient& mqtt_client, std::deque<OutboundMessage>& outbox) {
    while (mqtt_client.IsConnected() && !outbox.empty()) {
        if (!PublishOutbound(mqtt_client, outbox.front())) {
            return;
        }
        outbox.pop_front();
    }
}

[[nodiscard]] GripperReport MakeUartLinkStatus(std::string current_state, std::optional<std::string> error_code) {
    return {
        .channel = GripperReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = error_code.has_value() ? mqtt::ConnectionState::kUartError : mqtt::ConnectionState::kOnline,
                .current_state = std::move(current_state),
                .job_id = std::nullopt,
                .error_code = std::move(error_code),
            },
    };
}

int RunGripperDaemon(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "usage: logistics_gripper_node [node.ini] [/dev/vedauart]\n";
        return 2;
    }

    const std::filesystem::path config_path = ResolveConfigPath(argc, argv);

    MqttNodeConfig config;
    try {
        config = LoadMqttNodeConfig(config_path);
    } catch (const NodeConfigError& error) {
        std::cerr << "[gripper][ERROR] " << error.what() << '\n';
        return 1;
    }

    GripperPoseConfig poses;
    try {
        poses = LoadGripperPoseConfig(config_path);
    } catch (const GripperConfigError& error) {
        std::cerr << "[gripper][ERROR] " << error.what() << '\n';
        return 1;
    }

    const std::string device_id = config.device_id;
    const std::string uart_path = ResolveUartPath(argc, argv);
    auto device_status = std::make_shared<DeviceStatus>(device_id);
    InputUartSession uart_session;
    GripperNode gripper_node(device_id, uart_session, poses);
    MqttNodeClient mqtt_client(std::move(config), std::string(contracts::ToString(contracts::DeviceRole::kGripper)),
                               device_status);

    CommandInbox command_inbox;
    std::deque<OutboundMessage> outbox;
    const std::string message_session_id = GenerateMessageSessionId();
    std::uint64_t message_sequence = 1U;

    const auto queue_report = [&](const GripperReport& report) {
        static_cast<void>(EnqueueOutbound(outbox, report, device_id, message_session_id, message_sequence,
                                          *device_status, &mqtt_client));
    };
    gripper_node.SetReportHandler(queue_report);
    uart_session.SetSpontaneousFrameHandler(
        [&gripper_node](const uart_frame_t& frame) { gripper_node.HandleUartFrame(frame); });
    mqtt_client.SetCommandHandler([&command_inbox](const mqtt::MqttMessage& message) {
        if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message)) {
            std::string work_id;
            const auto work_id_it = command->params.find("workId");
            if (work_id_it != command->params.end() && work_id_it->is_string()) {
                work_id = work_id_it->get<std::string>();
            }
            std::clog << "[gripper][mqtt][INFO] command received: message_id=" << message.message_id
                      << "; request_id=" << command->request_id << "; command=" << mqtt::ToString(command->command)
                      << "; target=" << command->target_device_id << "; component=" << command->component_id
                      << "; work_id=" << work_id << '\n';
        } else {
            std::clog << "[gripper][mqtt][INFO] non-control command received: message_id=" << message.message_id
                      << '\n';
        }
        const bool accepted = command_inbox.Push(message);
        if (!accepted) {
            std::cerr << "[gripper][mqtt][ERROR] command queue full; command rejected: " << message.message_id << '\n';
        }
        return accepted;
    });

    device_status->SetUartConnected(false);
    device_status->SetCurrentState("STARTING");
    if (!mqtt_client.Start()) {
        return 1;
    }

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    auto next_uart_reconnect = Clock::now();
    auto next_heartbeat = Clock::now();
    auto last_tick = Clock::now();
    std::clog << "[gripper][INFO] daemon started: id=" << device_id << "; uart=" << uart_path << '\n';

    while (stop_requested == 0) {
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
        last_tick = now;

        if (!uart_session.IsOpen() && now >= next_uart_reconnect) {
            if (uart_session.Open(uart_path)) {
                gripper_node.ResetControllerHeartbeatMonitor();
                device_status->SetUartConnected(true);
                queue_report(MakeUartLinkStatus("UART_CONNECTED", std::nullopt));
                std::clog << "[gripper][uart][INFO] connected: " << uart_path << '\n';
                const auto startup_home = gripper_node.InitializeAtStartup();
                std::clog << "[gripper][startup][" << (startup_home.Succeeded() ? "INFO" : "ERROR")
                          << "] home requested: accepted=" << (startup_home.Succeeded() ? "true" : "false")
                          << "; status=" << static_cast<int>(startup_home.status) << '\n';
            } else {
                next_uart_reconnect = now + kUartReconnectInterval;
            }
        }

        const bool was_open = uart_session.IsOpen();

        for (const mqtt::MqttMessage& command : command_inbox.TakeAll()) {
            const device::GripperCommandResult result = gripper_node.HandleMqttCommand(command);
            std::clog << "[gripper][command][" << (result.Succeeded() ? "INFO" : "WARN")
                      << "] command handled: request_id=" << result.request_id << "; work_id=" << result.work_id
                      << "; accepted=" << (result.Succeeded() ? "true" : "false")
                      << "; status=" << static_cast<int>(result.status)
                      << "; uart_command=" << static_cast<unsigned>(result.uart_command)
                      << "; motion_id=" << result.motion_id
                      << "; uart_status=" << static_cast<int>(result.uart_result.status) << '\n';
        }

        if (uart_session.IsOpen()) {
            // Motion completions arrive unsolicited, so the spontaneous poll is what
            // actually drives the cycle forward between commands.
            static_cast<void>(uart_session.PollSpontaneous(kSpontaneousPollTimeout));
            gripper_node.Tick(elapsed);
        }

        if (was_open && !uart_session.IsOpen()) {
            device_status->SetUartConnected(false);
            queue_report(MakeUartLinkStatus("UART_DISCONNECTED", std::string("ERR-UART-DISCONNECTED")));
            next_uart_reconnect = Clock::now() + kUartReconnectInterval;
            std::cerr << "[gripper][uart][WARN] disconnected; reconnect scheduled\n";
        }

        FlushOutbox(mqtt_client, outbox);

        if (mqtt_client.IsConnected() && now >= next_heartbeat) {
            static_cast<void>(mqtt_client.PublishHeartbeat(
                MakeMessageId(device_id, message_session_id, message_sequence++), CurrentIso8601Timestamp()));
            next_heartbeat = now + mqtt::kHeartbeatInterval;
        }
        std::this_thread::sleep_for(kIdleDelay);
    }

    uart_session.Close();
    device_status->SetUartConnected(false);
    mqtt_client.Stop();
    std::clog << "[gripper][INFO] daemon stopped\n";
    return 0;
}

}  // namespace
}  // namespace logistics::device

int main(int argc, char* argv[]) {
    return logistics::device::RunGripperDaemon(argc, argv);
}

#else

int main(int argc, char* argv[]) {
    return logistics::device::NodeRuntime{ logistics::contracts::DeviceRole::kGripper }.Run(argc, argv);
}

#endif
