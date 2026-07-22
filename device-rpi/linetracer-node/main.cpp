#include "logistics/device/node_runtime.hpp"

#ifdef LOGISTICS_LINETRACER_DAEMON_ENABLED

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
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
#include "logistics/device/linetracer_node.hpp"
#include "logistics/device/mqtt_node_client.hpp"
#include "logistics/device/mqtt_node_config.hpp"
#include "logistics/device/mqtt_time.hpp"
#include "logistics/device/uart_session.hpp"
#include "logistics/device/uart_transport.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;
using Clock = std::chrono::steady_clock;

inline constexpr std::size_t kCommandQueueCapacity = 64U;
inline constexpr std::size_t kOutboundQueueCapacity = 256U;
inline constexpr auto kUartPollTimeout = std::chrono::milliseconds{ 5 };
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
    LineTracerReportChannel channel{ LineTracerReportChannel::kStatus };
    mqtt::MqttMessage message;
};

[[nodiscard]] OutboundMessage MakeOutboundMessage(const LineTracerReport& report, std::string_view device_id,
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

void UpdateDeviceStatus(const LineTracerReport& report, const std::shared_ptr<DeviceStatus>& device_status) {
    if (const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&report.data); status != nullptr) {
        device_status->SetCurrentState(status->current_state);
        device_status->SetJobId(status->job_id);
        device_status->SetErrorCode(status->error_code);
        return;
    }
    if (const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&report.data); error != nullptr) {
        device_status->SetCurrentState(error->current_state);
        device_status->SetJobId(error->job_id);
        device_status->SetErrorCode(error->error_code);
    }
}

[[nodiscard]] bool EnqueueOutbound(std::deque<OutboundMessage>& outbox, const LineTracerReport& report,
                                   std::string_view device_id, std::string_view message_session_id,
                                   std::uint64_t& message_sequence,
                                   const std::shared_ptr<DeviceStatus>& device_status) {
    UpdateDeviceStatus(report, device_status);
    if (outbox.size() >= kOutboundQueueCapacity) {
        const auto stale_status = std::find_if(outbox.begin(), outbox.end(), [](const OutboundMessage& queued) {
            return queued.channel == LineTracerReportChannel::kStatus;
        });
        if (stale_status == outbox.end()) {
            std::cerr << "[linetracer][mqtt][ERROR] outbound queue full; preserving queued responses and events\n";
            return false;
        }
        outbox.erase(stale_status);
    }
    outbox.push_back(MakeOutboundMessage(report, device_id, message_session_id, message_sequence));
    return true;
}

[[nodiscard]] bool PublishOutbound(MqttNodeClient& mqtt_client, const OutboundMessage& outbound) {
    switch (outbound.channel) {
        case LineTracerReportChannel::kResponse:
            return mqtt_client.PublishResponse(outbound.message);
        case LineTracerReportChannel::kStatus:
            return mqtt_client.PublishStatus(outbound.message);
        case LineTracerReportChannel::kEvent:
            return mqtt_client.PublishEvent(outbound.message);
        case LineTracerReportChannel::kError:
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

[[nodiscard]] mqtt::CommandResult LocalCommandResult(LineTracerCommandStatus status) noexcept {
    switch (status) {
        case LineTracerCommandStatus::kInvalidTarget:
        case LineTracerCommandStatus::kInvalidDestination:
        case LineTracerCommandStatus::kNoActiveJob:
        case LineTracerCommandStatus::kUnsupportedMessage:
        case LineTracerCommandStatus::kUnsupportedCommand:
        case LineTracerCommandStatus::kUartBusy:
            return mqtt::CommandResult::kRejected;
        case LineTracerCommandStatus::kInvalidMessage:
        case LineTracerCommandStatus::kUartNotOpen:
        case LineTracerCommandStatus::kUartError:
            return mqtt::CommandResult::kFailed;
        case LineTracerCommandStatus::kSent:
            return mqtt::CommandResult::kProcessing;
    }
    return mqtt::CommandResult::kFailed;
}

[[nodiscard]] std::string LocalCommandError(LineTracerCommandStatus status) {
    switch (status) {
        case LineTracerCommandStatus::kInvalidMessage:
            return "ERR-MQTT-INVALID-MESSAGE";
        case LineTracerCommandStatus::kInvalidTarget:
            return "ERR-MQTT-INVALID-TARGET";
        case LineTracerCommandStatus::kInvalidDestination:
            return "ERR-MQTT-INVALID-DESTINATION";
        case LineTracerCommandStatus::kNoActiveJob:
            return "ERR-NO-ACTIVE-JOB";
        case LineTracerCommandStatus::kUnsupportedMessage:
        case LineTracerCommandStatus::kUnsupportedCommand:
            return "ERR-UNSUPPORTED-COMMAND";
        case LineTracerCommandStatus::kUartNotOpen:
            return "ERR-UART-DISCONNECTED";
        case LineTracerCommandStatus::kUartBusy:
            return "ERR-UART-BUSY";
        case LineTracerCommandStatus::kUartError:
            return "ERR-UART-IO";
        case LineTracerCommandStatus::kSent:
            return {};
    }
    return "ERR-INTERNAL";
}

[[nodiscard]] std::optional<LineTracerReport> MakeLocalCommandResponse(const LineTracerCommandResult& result) {
    if (result.Succeeded() || result.request_id.empty() || result.mqtt_command == mqtt::ControlCommand::kUnknown) {
        return std::nullopt;
    }

    const std::string error = LocalCommandError(result.status);
    return LineTracerReport{
        .channel = LineTracerReportChannel::kResponse,
        .message_type = mqtt::MessageType::kCommandResponse,
        .data =
            mqtt::CommandResponsePayload{
                .request_id = result.request_id,
                .command = result.mqtt_command,
                .result = LocalCommandResult(result.status),
                .error_code = error.empty() ? std::nullopt : std::optional<std::string>{ error },
                .message = "line tracer command was rejected before UART acknowledgement",
            },
    };
}

[[nodiscard]] LineTracerReport MakeUartStatus(std::string current_state, std::optional<std::string> job_id,
                                              std::optional<std::string> error_code) {
    return {
        .channel = LineTracerReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = error_code.has_value() ? mqtt::ConnectionState::kUartError : mqtt::ConnectionState::kOnline,
                .current_state = std::move(current_state),
                .job_id = std::move(job_id),
                .error_code = std::move(error_code),
            },
    };
}

[[nodiscard]] std::optional<std::string> ActiveWorkId(const LineTracerNode& node) {
    if (!node.HasActiveJob()) {
        return std::nullopt;
    }
    return std::string(node.ActiveWorkId());
}

int RunLineTracerDaemon(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "usage: logistics_linetracer_node [node.ini] [/dev/vedauart]\n";
        return 2;
    }

    MqttNodeConfig config;
    try {
        config = LoadMqttNodeConfig(ResolveConfigPath(argc, argv));
    } catch (const NodeConfigError& error) {
        std::cerr << "[linetracer][ERROR] " << error.what() << '\n';
        return 1;
    }

    const std::string device_id = config.device_id;
    const std::string uart_path = ResolveUartPath(argc, argv);
    auto device_status = std::make_shared<DeviceStatus>(device_id);
    UartSession uart_session;
    LineTracerNode line_tracer(device_id, uart_session);
    MqttNodeClient mqtt_client(std::move(config), std::string(contracts::ToString(contracts::DeviceRole::kLineTracer)),
                               device_status);

    CommandInbox command_inbox;
    std::deque<OutboundMessage> outbox;
    const std::string message_session_id = GenerateMessageSessionId();
    std::uint64_t message_sequence = 1U;
    bool uart_failure_pending = false;
    bool uart_disconnected_reported = false;

    const auto queue_report = [&](const LineTracerReport& report) {
        static_cast<void>(
            EnqueueOutbound(outbox, report, device_id, message_session_id, message_sequence, device_status));
    };
    line_tracer.SetReportHandler(queue_report);
    uart_session.SetEventHandler([&](const UartSessionEvent& event) {
        line_tracer.HandleUartEvent(event);
        if (event.type == UartSessionEventType::kTransportDisconnected ||
            event.type == UartSessionEventType::kTransportError) {
            uart_failure_pending = true;
        }
    });
    mqtt_client.SetCommandHandler([&command_inbox](const mqtt::MqttMessage& message) {
        if (!command_inbox.Push(message)) {
            std::cerr << "[linetracer][mqtt][ERROR] command queue full; command rejected: " << message.message_id
                      << '\n';
        }
    });

    device_status->SetUartConnected(false);
    if (!mqtt_client.Start()) {
        return 1;
    }

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    auto last_tick = Clock::now();
    auto next_uart_reconnect = last_tick;
    auto next_heartbeat = last_tick;
    std::clog << "[linetracer][INFO] daemon started: id=" << device_id << "; uart=" << uart_path << '\n';

    while (stop_requested == 0) {
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
        last_tick = now;
        uart_session.Tick(elapsed);

        if (!uart_session.IsOpen() && now >= next_uart_reconnect) {
            if (uart_session.Open(uart_path)) {
                device_status->SetUartConnected(true);
                uart_disconnected_reported = false;
                uart_failure_pending = false;
                queue_report(MakeUartStatus(line_tracer.HasActiveJob() ? "UART_RECONNECTED" : "IDLE",
                                            ActiveWorkId(line_tracer), std::nullopt));
                std::clog << "[linetracer][uart][INFO] connected: " << uart_path << '\n';
            } else {
                uart_failure_pending = true;
                next_uart_reconnect = now + kUartReconnectInterval;
            }
        }

        if (uart_session.IsOpen()) {
            const UartIoResult read_result = uart_session.PollOnce(kUartPollTimeout);
            if (read_result.status == UartIoStatus::kDisconnected || read_result.status == UartIoStatus::kNotOpen ||
                read_result.status == UartIoStatus::kIoError) {
                uart_failure_pending = true;
            }
        }

        for (const mqtt::MqttMessage& command : command_inbox.TakeAll()) {
            const LineTracerCommandResult result = line_tracer.HandleMqttCommand(command);
            if (const auto response = MakeLocalCommandResponse(result); response.has_value()) {
                queue_report(*response);
            }
        }

        if (uart_failure_pending && !uart_disconnected_reported) {
            uart_session.Close();
            device_status->SetUartConnected(false);
            queue_report(
                MakeUartStatus("UART_DISCONNECTED", ActiveWorkId(line_tracer), std::string("ERR-UART-DISCONNECTED")));
            uart_disconnected_reported = true;
            uart_failure_pending = false;
            next_uart_reconnect = Clock::now() + kUartReconnectInterval;
            std::cerr << "[linetracer][uart][WARN] disconnected; reconnect scheduled\n";
        }

        device_status->SetJobId(ActiveWorkId(line_tracer));
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
    std::clog << "[linetracer][INFO] daemon stopped\n";
    return 0;
}

}  // namespace
}  // namespace logistics::device

int main(int argc, char* argv[]) {
    return logistics::device::RunLineTracerDaemon(argc, argv);
}

#else

int main(int argc, char* argv[]) {
    return logistics::device::NodeRuntime{ logistics::contracts::DeviceRole::kLineTracer }.Run(argc, argv);
}

#endif
