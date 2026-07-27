#include "logistics/device/node_runtime.hpp"

#ifdef LOGISTICS_INPUT_DAEMON_ENABLED

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
#include "logistics/device/input_node.hpp"
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
    if (const auto* error = std::get_if<mqtt::ErrorOccurredPayload>(&report.data); error != nullptr) {
        // An async error/event updates the active error code but must not overwrite
        // the operational current_state (conveyor/sensor state from DeviceStatus
        // reports); otherwise a transient controller event would mask the real
        // state in the heartbeat until the next operational update.
        device_status.SetErrorCode(error->error_code);
    }
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
            std::cerr << "[input][mqtt][ERROR] outbound queue full; preserving queued responses and events\n";
            return false;
        }
        outbox.erase(stale_status);
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

    CommandInbox command_inbox;
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
    mqtt_client.SetCommandHandler([&command_inbox](const mqtt::MqttMessage& message) {
        if (!command_inbox.Push(message)) {
            std::cerr << "[input][mqtt][ERROR] command queue full; command rejected: " << message.message_id << '\n';
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
    auto next_heartbeat = Clock::now();
    std::clog << "[input][INFO] daemon started: id=" << device_id << "; uart=" << uart_path << '\n';

    while (stop_requested == 0) {
        const auto now = Clock::now();

        if (!uart_session.IsOpen() && now >= next_uart_reconnect) {
            if (uart_session.Open(uart_path)) {
                device_status->SetUartConnected(true);
                queue_report(MakeUartLinkStatus("UART_CONNECTED", std::nullopt));
                std::clog << "[input][uart][INFO] connected: " << uart_path << '\n';
            } else {
                next_uart_reconnect = now + kUartReconnectInterval;
            }
        }

        const bool was_open = uart_session.IsOpen();

        for (const mqtt::MqttMessage& command : command_inbox.TakeAll()) {
            static_cast<void>(input_node.HandleMqttCommand(command));
        }

        if (uart_session.IsOpen()) {
            static_cast<void>(uart_session.PollSpontaneous(kSpontaneousPollTimeout));
        }

        if (was_open && !uart_session.IsOpen()) {
            device_status->SetUartConnected(false);
            queue_report(MakeUartLinkStatus("UART_DISCONNECTED", std::string("ERR-UART-DISCONNECTED")));
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
