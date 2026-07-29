#include "logistics/device/node_runtime.hpp"

#ifdef LOGISTICS_SORTING_DAEMON_ENABLED

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "logistics/contracts/device.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/device/device_status.hpp"
#include "logistics/device/mqtt_node_client.hpp"
#include "logistics/device/mqtt_node_config.hpp"
#include "logistics/device/mqtt_time.hpp"
#include "logistics/device/node_command_queue.hpp"
#include "logistics/device/sorting_node.hpp"
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

struct OutboundMessage {
    SortingReportChannel channel{ SortingReportChannel::kStatus };
    mqtt::MqttMessage message;
};

[[nodiscard]] OutboundMessage MakeOutboundMessage(const SortingReport& report, std::string_view device_id,
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

void UpdateDeviceStatus(const SortingReport& report, const std::shared_ptr<DeviceStatus>& device_status) {
    if (const auto* status = std::get_if<mqtt::DeviceStatusPayload>(&report.data); status != nullptr) {
        device_status->SetCurrentState(status->current_state);
        device_status->SetJobId(status->job_id);
        device_status->SetErrorCode(status->error_code);
        return;
    }
    if (const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&report.data); error != nullptr) {
        device_status->SetErrorCode(error->error_code);
    }
}

[[nodiscard]] bool EnqueueOutbound(std::deque<OutboundMessage>& outbox, const SortingReport& report,
                                   std::string_view device_id, std::string_view message_session_id,
                                   std::uint64_t& message_sequence,
                                   const std::shared_ptr<DeviceStatus>& device_status) {
    UpdateDeviceStatus(report, device_status);
    if (const auto* sensor = std::get_if<mqtt::SensorStatusPayload>(&report.data); sensor != nullptr) {
        const auto older_measurement =
            std::find_if(outbox.begin(), outbox.end(), [sensor](const OutboundMessage& queued) {
                const auto* queued_sensor = std::get_if<mqtt::SensorStatusPayload>(&queued.message.data);
                return queued_sensor != nullptr && queued_sensor->sensor_id == sensor->sensor_id;
            });
        if (older_measurement != outbox.end()) {
            outbox.erase(older_measurement);
        }
    }
    if (outbox.size() >= kOutboundQueueCapacity) {
        const auto stale_status = std::find_if(outbox.begin(), outbox.end(), [](const OutboundMessage& queued) {
            return queued.channel == SortingReportChannel::kStatus ||
                   queued.message.message_type == mqtt::MessageType::kSensorStatus;
        });
        if (stale_status == outbox.end()) {
            if (report.channel != SortingReportChannel::kResponse) {
                std::cerr << "[sorting][mqtt][ERROR] outbound queue full; preserving queued command responses\n";
                return false;
            }
            std::cerr << "[sorting][mqtt][WARN] outbound queue capacity exceeded to preserve a command response\n";
        } else {
            outbox.erase(stale_status);
        }
    }
    outbox.push_back(MakeOutboundMessage(report, device_id, message_session_id, message_sequence));
    return true;
}

[[nodiscard]] bool PublishOutbound(MqttNodeClient& mqtt_client, const OutboundMessage& outbound) {
    switch (outbound.channel) {
        case SortingReportChannel::kResponse:
            return mqtt_client.PublishResponse(outbound.message);
        case SortingReportChannel::kStatus:
            return mqtt_client.PublishStatus(outbound.message);
        case SortingReportChannel::kEvent:
            return mqtt_client.PublishEvent(outbound.message);
        case SortingReportChannel::kError:
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

[[nodiscard]] mqtt::CommandResult LocalCommandResult(SortingCommandStatus status) noexcept {
    switch (status) {
        case SortingCommandStatus::kDuplicate:
            return mqtt::CommandResult::kDuplicated;
        case SortingCommandStatus::kInvalidTarget:
        case SortingCommandStatus::kInvalidDestination:
        case SortingCommandStatus::kInvalidSpeed:
        case SortingCommandStatus::kActiveCycleConflict:
        case SortingCommandStatus::kNoActiveCycle:
        case SortingCommandStatus::kUnsupportedMessage:
        case SortingCommandStatus::kUnsupportedCommand:
        case SortingCommandStatus::kUartBusy:
            return mqtt::CommandResult::kRejected;
        case SortingCommandStatus::kInvalidMessage:
        case SortingCommandStatus::kUartNotOpen:
        case SortingCommandStatus::kUartError:
            return mqtt::CommandResult::kFailed;
        case SortingCommandStatus::kSent:
            return mqtt::CommandResult::kProcessing;
        case SortingCommandStatus::kSentNoReply:
            return mqtt::CommandResult::kProcessing;
    }
    return mqtt::CommandResult::kFailed;
}

[[nodiscard]] std::optional<std::string> LocalCommandError(SortingCommandStatus status) {
    switch (status) {
        case SortingCommandStatus::kSent:
        case SortingCommandStatus::kSentNoReply:
        case SortingCommandStatus::kDuplicate:
            return std::nullopt;
        case SortingCommandStatus::kInvalidMessage:
            return "ERR-MQTT-INVALID-MESSAGE";
        case SortingCommandStatus::kInvalidTarget:
            return "ERR-MQTT-INVALID-TARGET";
        case SortingCommandStatus::kInvalidDestination:
            return "ERR-MQTT-INVALID-DESTINATION";
        case SortingCommandStatus::kInvalidSpeed:
            return "ERR-SPEED-REQUIRED";
        case SortingCommandStatus::kActiveCycleConflict:
            return "ERR-ACTIVE-CYCLE-CONFLICT";
        case SortingCommandStatus::kNoActiveCycle:
            return "ERR-NO-ACTIVE-CYCLE";
        case SortingCommandStatus::kUnsupportedMessage:
        case SortingCommandStatus::kUnsupportedCommand:
            return "ERR-UNSUPPORTED-COMMAND";
        case SortingCommandStatus::kUartNotOpen:
            return "ERR-UART-DISCONNECTED";
        case SortingCommandStatus::kUartBusy:
            return "ERR-UART-BUSY";
        case SortingCommandStatus::kUartError:
            return "ERR-UART-IO";
    }
    return "ERR-INTERNAL";
}

[[nodiscard]] std::optional<SortingReport> MakeLocalCommandResponse(const SortingCommandResult& result) {
    if (result.status == SortingCommandStatus::kSent || result.request_id.empty() ||
        result.mqtt_command == mqtt::ControlCommand::kUnknown) {
        return std::nullopt;
    }

    return SortingReport{
        .channel = SortingReportChannel::kResponse,
        .message_type = mqtt::MessageType::kCommandResponse,
        .data =
            mqtt::CommandResponsePayload{
                .request_id = result.request_id,
                .command = result.mqtt_command,
                .result = LocalCommandResult(result.status),
                .error_code = LocalCommandError(result.status),
                .message = result.status == SortingCommandStatus::kDuplicate
                               ? "sorting command already applied; motor action was not repeated"
                           : result.status == SortingCommandStatus::kSentNoReply
                               ? "sorting safety command was sent once; controller state follows asynchronously"
                               : "sorting command was rejected before STM32 completion",
            },
    };
}

[[nodiscard]] SortingReport MakeUartStatus(std::string current_state, std::optional<std::string> work_id,
                                           std::optional<std::string> error_code) {
    return {
        .channel = SortingReportChannel::kStatus,
        .message_type = mqtt::MessageType::kDeviceStatus,
        .data =
            mqtt::DeviceStatusPayload{
                .status = error_code.has_value() ? mqtt::ConnectionState::kUartError : mqtt::ConnectionState::kOnline,
                .current_state = std::move(current_state),
                .job_id = std::move(work_id),
                .error_code = std::move(error_code),
            },
    };
}

[[nodiscard]] std::optional<std::string> ActiveWorkId(const SortingNode& node) {
    return node.HasActiveCycle() && !node.ActiveWorkId().empty()
               ? std::optional<std::string>{ std::string(node.ActiveWorkId()) }
               : std::nullopt;
}

int RunSortingDaemon(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "usage: logistics_sorting_node [node.ini] [/dev/vedauart]\n";
        return 2;
    }

    MqttNodeConfig config;
    try {
        config = LoadMqttNodeConfig(ResolveConfigPath(argc, argv));
    } catch (const NodeConfigError& error) {
        std::cerr << "[sorting][ERROR] " << error.what() << '\n';
        return 1;
    }

    const std::string device_id = config.device_id;
    const std::string uart_path = ResolveUartPath(argc, argv);
    auto device_status = std::make_shared<DeviceStatus>(device_id);
    UartSession uart_session;
    SortingNode sorting_node(device_id, uart_session);
    MqttNodeClient mqtt_client(std::move(config), std::string(contracts::ToString(contracts::DeviceRole::kSorting)),
                               device_status);

    NodeCommandQueue command_inbox(kCommandQueueCapacity);
    std::deque<OutboundMessage> outbox;
    const std::string message_session_id = GenerateMessageSessionId();
    std::uint64_t message_sequence = 1U;
    bool uart_failure_pending = false;
    bool uart_disconnected_reported = false;
    bool uart_resync_pending = false;

    const auto queue_report = [&](const SortingReport& report) {
        static_cast<void>(
            EnqueueOutbound(outbox, report, device_id, message_session_id, message_sequence, device_status));
    };
    sorting_node.SetReportHandler(queue_report);
    uart_session.SetEventHandler([&](const UartSessionEvent& event) {
        sorting_node.HandleUartEvent(event);
        if (event.type == UartSessionEventType::kTransportDisconnected ||
            event.type == UartSessionEventType::kTransportError) {
            uart_failure_pending = true;
        } else if (event.type == UartSessionEventType::kAckTimeout) {
            if (event.pending_command == UART_CMD_SORTING_GET_STATUS) {
                uart_resync_pending = false;
                uart_failure_pending = true;
            } else {
                uart_resync_pending = true;
            }
        }
    });
    mqtt_client.SetCommandHandler([&command_inbox, &mqtt_client, &device_id](const mqtt::MqttMessage& message) {
        if (!command_inbox.Push(message)) {
            std::cerr << "[sorting][mqtt][ERROR] command queue full; command rejected: " << message.message_id << '\n';
            const auto response = MakeTerminalCommandResponse(
                message, device_id, message.message_id + "-QUEUE-FULL", CurrentIso8601Timestamp(),
                mqtt::CommandResult::kRejected, std::string("ERR-COMMAND-QUEUE-FULL"),
                "sorting command rejected because the local command queue is full");
            if (!response.has_value() || !mqtt_client.PublishResponse(*response)) {
                std::cerr << "[sorting][mqtt][ERROR] unable to publish command queue full response: "
                          << message.message_id << '\n';
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

    auto last_tick = Clock::now();
    auto next_uart_reconnect = last_tick;
    auto next_heartbeat = last_tick;
    std::clog << "[sorting][INFO] daemon started: id=" << device_id << "; uart=" << uart_path << '\n';

    while (stop_requested == 0) {
        bool emergency_processed = false;
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
        last_tick = now;
        uart_session.Tick(elapsed);

        if (!uart_session.IsOpen() && now >= next_uart_reconnect) {
            if (uart_session.Open(uart_path)) {
                sorting_node.ResetControllerHeartbeatMonitor();
                device_status->SetUartConnected(true);
                uart_disconnected_reported = false;
                uart_failure_pending = false;
                queue_report(MakeUartStatus("UART_RECONNECTED", ActiveWorkId(sorting_node), std::nullopt));
                const SortingCommandResult status_result = sorting_node.RequestControllerStatus();
                if (!status_result.Succeeded()) {
                    uart_failure_pending = status_result.status != SortingCommandStatus::kUartBusy;
                } else {
                    uart_resync_pending = false;
                }
                std::clog << "[sorting][uart][INFO] connected: " << uart_path << '\n';
            } else {
                uart_failure_pending = true;
                next_uart_reconnect = now + kUartReconnectInterval;
            }
        }

        if (!uart_session.IsOpen()) {
            const auto reject_disconnected = [&](const mqtt::MqttMessage& command) {
                const auto response = MakeTerminalCommandResponse(
                    command, device_id, command.message_id + "-UART-DISCONNECTED", CurrentIso8601Timestamp(),
                    mqtt::CommandResult::kFailed, std::string("ERR-UART-DISCONNECTED"),
                    "sorting command failed because the STM32 UART is disconnected");
                if (!response.has_value() || !mqtt_client.PublishResponse(*response)) {
                    std::cerr << "[sorting][mqtt][ERROR] unable to publish UART disconnected response: "
                              << command.message_id << '\n';
                }
            };
            while (auto command = command_inbox.TryPopEmergency()) {
                reject_disconnected(*command);
            }
            while (auto command = command_inbox.TryPop()) {
                reject_disconnected(*command);
            }
        }

        if (uart_session.IsOpen()) {
            if (auto command = command_inbox.TryPopEmergency(); command.has_value()) {
                const SortingCommandResult result = sorting_node.HandleMqttCommand(*command);
                if (const auto response = MakeLocalCommandResponse(result); response.has_value()) {
                    queue_report(*response);
                }
                emergency_processed = true;
            }
        }

        if (uart_session.IsOpen()) {
            const UartIoResult read_result = uart_session.PollOnce(kUartPollTimeout);
            sorting_node.Tick(elapsed);
            if (read_result.status == UartIoStatus::kDisconnected || read_result.status == UartIoStatus::kNotOpen ||
                read_result.status == UartIoStatus::kIoError) {
                uart_failure_pending = true;
            }
        }

        if (!emergency_processed && !uart_failure_pending && uart_session.IsOpen() &&
            !uart_session.HasPendingCommand() && uart_resync_pending) {
            const SortingCommandResult status_result = sorting_node.RequestControllerStatus();
            if (status_result.Succeeded()) {
                uart_resync_pending = false;
            } else if (status_result.status != SortingCommandStatus::kUartBusy) {
                uart_failure_pending = true;
            }
        }

        if (!emergency_processed && !uart_failure_pending && uart_session.IsOpen() &&
            !uart_session.HasPendingCommand() && !uart_resync_pending) {
            if (auto command = command_inbox.TryPop(); command.has_value()) {
                const SortingCommandResult result = sorting_node.HandleMqttCommand(*command);
                if (const auto response = MakeLocalCommandResponse(result); response.has_value()) {
                    queue_report(*response);
                }
            }
        }

        if (uart_failure_pending && !uart_disconnected_reported) {
            uart_session.Close();
            device_status->SetUartConnected(false);
            queue_report(
                MakeUartStatus("UART_DISCONNECTED", ActiveWorkId(sorting_node), std::string("ERR-UART-DISCONNECTED")));
            uart_disconnected_reported = true;
            uart_failure_pending = false;
            next_uart_reconnect = Clock::now() + kUartReconnectInterval;
            std::cerr << "[sorting][uart][WARN] disconnected; reconnect scheduled\n";
        }

        device_status->SetJobId(ActiveWorkId(sorting_node));
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
    std::clog << "[sorting][INFO] daemon stopped\n";
    return 0;
}

}  // namespace
}  // namespace logistics::device

int main(int argc, char* argv[]) {
    return logistics::device::RunSortingDaemon(argc, argv);
}

#else

int main(int argc, char* argv[]) {
    return logistics::device::NodeRuntime{ logistics::contracts::DeviceRole::kSorting }.Run(argc, argv);
}

#endif
