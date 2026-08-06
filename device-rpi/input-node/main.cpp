#include "logistics/device/node_runtime.hpp"

#ifdef LOGISTICS_INPUT_DAEMON_ENABLED

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/device/device_status.hpp"
#include "logistics/device/input_node.hpp"
#include "logistics/device/input_uart_session.hpp"
#include "logistics/device/mqtt_node_client.hpp"
#include "logistics/device/mqtt_node_config.hpp"
#include "logistics/device/mqtt_time.hpp"
#include "logistics/device/node_command_queue.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;
using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kCommandQueueCapacity = 64U;
inline constexpr std::size_t kOutboundQueueCapacity = 256U;
inline constexpr auto kSpontaneousPollTimeout = std::chrono::milliseconds{ 20 };
inline constexpr auto kUartReconnectInterval = std::chrono::seconds{ 2 };
// 2000ms은 컨트롤러의 정확히 1000ms짜리 하트비트와 주기적으로 재정렬되어(비트
// 주파수) 약 8분 34초마다 응답이 지연되는 현상을 만들었다(2026-08-06 실기기
// 관측, ERR-HEALTH-QUEUE-OVERFLOW와 시각 일치 확인). 하트비트 주기의 작은
// 정수배에서 벗어난 값으로 바꿔 재정렬 주기를 늘린다 - 충돌 가능성 자체를
// 없애는 게 아니라 훨씬 뜸하게 만드는 완화책이다.
inline constexpr auto kUartKeepAliveInterval = std::chrono::milliseconds{ 2700 };
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

struct OutboundMessage {
    InputReportChannel channel{ InputReportChannel::kStatus };
    mqtt::MqttMessage message;
};

[[nodiscard]] OutboundMessage MakeOutboundMessage(const InputReport& report, std::string_view device_id,
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

void UpdateDeviceStatus(const InputReport& report, DeviceStatus& device_status) {
    if (const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&report.data); status != nullptr) {
        device_status.SetCurrentState(status->current_state);
        device_status.SetErrorCode(status->error_code);
        return;
    }
    // ERROR_OCCURRED is an event for the operational log, not an authoritative
    // snapshot of the node's active state. Persisting it in DeviceStatus would
    // repeat a transient error in every MQTT heartbeat until some unrelated
    // controller state transition emitted a new DEVICE_STATUS.
}

[[nodiscard]] bool EnqueueOutbound(std::deque<OutboundMessage>& outbox, const InputReport& report,
                                   std::string_view device_id, std::string_view message_session_id,
                                   std::uint64_t& message_sequence, DeviceStatus& device_status) {
    UpdateDeviceStatus(report, device_status);
    if (outbox.size() >= kOutboundQueueCapacity) {
        const auto stale_status = std::find_if(outbox.begin(), outbox.end(), [](const OutboundMessage& queued) {
            return queued.channel == InputReportChannel::kStatus;
        });
        if (stale_status == outbox.end()) {
            if (report.channel != InputReportChannel::kResponse) {
                std::cerr << "[input][mqtt][ERROR] outbound queue full; preserving queued command responses\n";
                return false;
            }
            std::cerr << "[input][mqtt][WARN] outbound queue capacity exceeded to preserve a command response\n";
        } else {
            outbox.erase(stale_status);
        }
    }
    outbox.push_back(MakeOutboundMessage(report, device_id, message_session_id, message_sequence));
    return true;
}

[[nodiscard]] bool PublishOutbound(MqttNodeClient& mqtt_client, const OutboundMessage& outbound) {
    switch (outbound.channel) {
        case InputReportChannel::kResponse:
            return mqtt_client.PublishResponse(outbound.message);
        case InputReportChannel::kStatus:
            return mqtt_client.PublishStatus(outbound.message);
        case InputReportChannel::kEvent:
            return mqtt_client.PublishEvent(outbound.message);
        case InputReportChannel::kError:
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

[[nodiscard]] InputReport MakeUartLinkStatus(std::string current_state, std::optional<std::string> error_code) {
    return {
        .channel = InputReportChannel::kStatus,
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

int RunInputDaemon(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "usage: logistics_input_node [node.ini] [/dev/vedauart]\n";
        return 2;
    }

    MqttNodeConfig config;
    try {
        config = LoadMqttNodeConfig(ResolveConfigPath(argc, argv));
    } catch (const NodeConfigError& error) {
        std::cerr << "[input][ERROR] " << error.what() << '\n';
        return 1;
    }

    const std::string device_id = config.device_id;
    const std::string uart_path = ResolveUartPath(argc, argv);
    auto device_status = std::make_shared<DeviceStatus>(device_id);
    InputUartSession uart_session;
    InputNode input_node(device_id, uart_session);
    MqttNodeClient mqtt_client(std::move(config), std::string(contracts::ToString(contracts::DeviceRole::kInput)),
                               device_status);

    NodeCommandQueue command_inbox(kCommandQueueCapacity);
    std::deque<OutboundMessage> outbox;
    const std::string message_session_id = GenerateMessageSessionId();
    std::uint64_t message_sequence = 1U;

    const auto queue_report = [&](const InputReport& report) {
        static_cast<void>(
            EnqueueOutbound(outbox, report, device_id, message_session_id, message_sequence, *device_status));
    };
    input_node.SetReportHandler(queue_report);
    uart_session.SetSpontaneousFrameHandler(
        [&input_node](const uart_frame_t& frame) { input_node.HandleUartFrame(frame); });
    mqtt_client.SetCommandHandler([&command_inbox, &mqtt_client, &device_id](const mqtt::MqttMessage& message) {
        std::deque<mqtt::MqttMessage> preempted;
        if (!command_inbox.Push(message, &preempted)) {
            std::cerr << "[input][mqtt][ERROR] command queue full; command rejected: " << message.message_id << '\n';
            const auto response = MakeTerminalCommandResponse(
                message, device_id, message.message_id + "-QUEUE-FULL", CurrentIso8601Timestamp(),
                mqtt::CommandResult::kRejected, std::string("ERR-COMMAND-QUEUE-FULL"),
                "input command rejected because the local command queue is full");
            if (!response.has_value() || !mqtt_client.PublishResponse(*response)) {
                std::cerr << "[input][mqtt][ERROR] unable to publish command queue full response: "
                          << message.message_id << '\n';
            }
        }
        for (const auto& command : preempted) {
            const auto response = MakeTerminalCommandResponse(
                command, device_id, command.message_id + "-ESTOP-PREEMPTED", CurrentIso8601Timestamp(),
                mqtt::CommandResult::kRejected, std::string("ERR-EMERGENCY-STOP-PREEMPTED"),
                "input command was preempted by an emergency stop");
            if (!response.has_value() || !mqtt_client.PublishResponse(*response)) {
                std::cerr << "[input][mqtt][ERROR] unable to publish emergency preemption response: "
                          << command.message_id << '\n';
            }
        }
    });

    device_status->SetUartConnected(false);
    if (!mqtt_client.Start()) {
        return 1;
    }

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    auto next_uart_reconnect = Clock::now();
    auto next_uart_keepalive = Clock::now();
    auto next_heartbeat = Clock::now();
    auto last_tick = Clock::now();
    bool uart_disconnected_reported = false;
    std::clog << "[input][INFO] daemon started: id=" << device_id << "; uart=" << uart_path << '\n';

    while (stop_requested == 0) {
        bool emergency_processed = false;
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
        last_tick = now;

        if (!uart_session.IsOpen() && now >= next_uart_reconnect) {
            if (uart_session.Open(uart_path)) {
                input_node.ResetControllerHeartbeatMonitor();
                next_uart_keepalive = now + kUartKeepAliveInterval;
                device_status->SetUartConnected(true);
                queue_report(MakeUartLinkStatus(uart_disconnected_reported ? "UART_RECONNECTED" : "UART_CONNECTED",
                                                std::nullopt));
                uart_disconnected_reported = false;
                std::clog << "[input][uart][INFO] connected: " << uart_path << '\n';
            } else {
                device_status->SetUartConnected(false);
                if (!uart_disconnected_reported) {
                    queue_report(MakeUartLinkStatus("UART_DISCONNECTED", std::string("ERR-UART-DISCONNECTED")));
                    uart_disconnected_reported = true;
                    std::cerr << "[input][uart][WARN] initial connection failed; reconnect scheduled\n";
                }
                next_uart_reconnect = now + kUartReconnectInterval;
            }
        }

        const bool was_open = uart_session.IsOpen();

        if (auto command = command_inbox.TryPopEmergency(); command.has_value()) {
            static_cast<void>(input_node.HandleMqttCommand(*command));
            emergency_processed = true;
        }
        if (!emergency_processed && !input_node.HasPendingSafetyCommand()) {
            if (auto command = command_inbox.TryPop(); command.has_value()) {
                static_cast<void>(input_node.HandleMqttCommand(*command));
            }
        }

        if (uart_session.IsOpen()) {
            static_cast<void>(uart_session.PollSpontaneous(kSpontaneousPollTimeout));
            input_node.Tick(elapsed);
        }

        if (uart_session.IsOpen() && now >= next_uart_keepalive) {
            const InputCommandResult keepalive = input_node.RequestControllerStatus();
            next_uart_keepalive = now + kUartKeepAliveInterval;
            if (keepalive.status == InputCommandStatus::kTimeout ||
                keepalive.status == InputCommandStatus::kUartNotOpen ||
                keepalive.status == InputCommandStatus::kUartError) {
                uart_session.Close();
            } else if (!keepalive.Succeeded()) {
                std::cerr << "[input][uart][WARN] keepalive rejected: status="
                          << static_cast<int>(keepalive.uart_result.response_status)
                          << "; error=" << static_cast<int>(keepalive.uart_result.response_error) << '\n';
            }
        }

        if (was_open && !uart_session.IsOpen() && !uart_disconnected_reported) {
            device_status->SetUartConnected(false);
            queue_report(MakeUartLinkStatus("UART_DISCONNECTED", std::string("ERR-UART-DISCONNECTED")));
            uart_disconnected_reported = true;
            next_uart_reconnect = Clock::now() + kUartReconnectInterval;
            std::cerr << "[input][uart][WARN] disconnected; reconnect scheduled\n";
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
    std::clog << "[input][INFO] daemon stopped\n";
    return 0;
}

}  // namespace
}  // namespace logistics::device

int main(int argc, char* argv[]) {
    return logistics::device::RunInputDaemon(argc, argv);
}

#else

int main(int argc, char* argv[]) {
    return logistics::device::NodeRuntime{ logistics::contracts::DeviceRole::kInput }.Run(argc, argv);
}

#endif
