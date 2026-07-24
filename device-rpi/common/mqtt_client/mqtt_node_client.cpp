#include "logistics/device/mqtt_node_client.hpp"

#include <mosquitto.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/device/mqtt_message_processor.hpp"
#include "logistics/device/mqtt_time.hpp"

namespace logistics::device {
namespace {

namespace mqtt = contracts::mqtt;

class MosquittoLibrary final {
public:
    MosquittoLibrary() : result_(mosquitto_lib_init()) {}

    ~MosquittoLibrary() {
        if (result_ == MOSQ_ERR_SUCCESS) {
            mosquitto_lib_cleanup();
        }
    }

    [[nodiscard]] int Result() const noexcept {
        return result_;
    }

private:
    int result_;
};

[[nodiscard]] MosquittoLibrary& Library() {
    static MosquittoLibrary library;
    return library;
}

[[nodiscard]] const char* ErrorText(int code) noexcept {
    const char* message = mosquitto_strerror(code);
    return message == nullptr ? "unknown Mosquitto error" : message;
}

[[nodiscard]] const char* ConnectionReasonText(int code) noexcept {
    const char* message = mosquitto_connack_string(code);
    return message == nullptr ? "unknown connection response" : message;
}

}  // namespace

class MqttNodeClient::Impl final {
public:
    Impl(MqttNodeConfig config, std::string device_type, std::shared_ptr<DeviceStatus> device_status)
        : config_(std::move(config)),
          device_type_(std::move(device_type)),
          processor_(config_.device_id),
          message_session_id_(GenerateMessageSessionId()),
          device_status_(std::move(device_status)) {
        if (device_status_ == nullptr || !device_status_->IsForDevice(config_.device_id)) {
            throw std::invalid_argument("device status must belong to the configured device ID");
        }
    }

    ~Impl() {
        Stop();
    }

    void SetCommandHandler(CommandHandler handler) {
        std::lock_guard lock(handler_mutex_);
        command_handler_ = std::move(handler);
    }

    [[nodiscard]] bool Start() {
        if (running_.exchange(true)) {
            return true;
        }
        if (!config_.IsValid() || device_type_.empty()) {
            running_ = false;
            std::cerr << "[device][mqtt][ERROR] invalid MQTT node configuration\n";
            return false;
        }
        device_status_->SetConnectionState(mqtt::ConnectionState::kReconnecting);
        if (Library().Result() != MOSQ_ERR_SUCCESS) {
            running_ = false;
            device_status_->SetConnectionState(mqtt::ConnectionState::kOffline);
            std::cerr << "[device][mqtt][ERROR] unable to initialize libmosquitto: " << ErrorText(Library().Result())
                      << '\n';
            return false;
        }

        client_ = mosquitto_new(config_.client_id.c_str(), config_.clean_session, this);
        if (client_ == nullptr) {
            running_ = false;
            device_status_->SetConnectionState(mqtt::ConnectionState::kOffline);
            std::cerr << "[device][mqtt][ERROR] unable to allocate Mosquitto client\n";
            return false;
        }

        mosquitto_connect_callback_set(client_, &Impl::OnConnected);
        mosquitto_disconnect_callback_set(client_, &Impl::OnDisconnected);
        mosquitto_message_callback_set(client_, &Impl::OnMessage);
        mosquitto_log_callback_set(client_, &Impl::OnLog);

        int result = mosquitto_int_option(client_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
        if (result == MOSQ_ERR_SUCCESS && !config_.username.empty()) {
            result = mosquitto_username_pw_set(client_, config_.username.c_str(),
                                               config_.password.empty() ? nullptr : config_.password.c_str());
        }
        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_reconnect_delay_set(client_, config_.reconnect_min_delay_seconds,
                                                   config_.reconnect_max_delay_seconds, true);
        }
        if (result == MOSQ_ERR_SUCCESS) {
            result = ConfigureLastWill();
        }
        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_connect_async(client_, config_.host.c_str(), config_.port, config_.keep_alive_seconds);
        }
        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_loop_start(client_);
            loop_started_ = result == MOSQ_ERR_SUCCESS;
        }
        if (result != MOSQ_ERR_SUCCESS) {
            std::cerr << "[device][mqtt][ERROR] unable to start MQTT connection: " << ErrorText(result) << '\n';
            Stop();
            return false;
        }

        std::clog << "[device][mqtt][INFO] MQTT connection started for " << config_.host << ':' << config_.port << '\n';
        return true;
    }

    void Stop() noexcept {
        if (!running_.exchange(false) && client_ == nullptr) {
            try {
                device_status_->SetConnectionState(mqtt::ConnectionState::kOffline);
            } catch (...) {
            }
            return;
        }
        const bool was_connected = connected_.exchange(false);
        const std::string timestamp = CurrentIso8601Timestamp();
        try {
            device_status_->SetConnectionState(mqtt::ConnectionState::kOffline);
        } catch (...) {
        }
        if (client_ == nullptr) {
            return;
        }

        bool offline_queued = false;
        if (was_connected) {
            try {
                const auto offline =
                    processor_.EncodeOfflineStatus(LifecycleMessageId("OFFLINE", connection_generation_), timestamp);
                offline_queued =
                    PublishEncoded(mqtt::DeviceStatusTopic(config_.device_id), offline, 1, true, "offline status");
                if (offline_queued) {
                    device_status_->MarkCommunication(timestamp);
                }
            } catch (...) {
                std::cerr << "[device][mqtt][ERROR] unable to prepare offline status during shutdown\n";
            }
        }

        const int disconnect_result = offline_queued ? mosquitto_disconnect(client_) : MOSQ_ERR_NO_CONN;
        const bool graceful_disconnect_started = disconnect_result == MOSQ_ERR_SUCCESS;
        if (loop_started_) {
            static_cast<void>(mosquitto_loop_stop(client_, !graceful_disconnect_started));
            loop_started_ = false;
        }
        mosquitto_destroy(client_);
        client_ = nullptr;
    }

    [[nodiscard]] bool IsConnected() const noexcept {
        return connected_;
    }

    [[nodiscard]] bool PublishHeartbeat(std::string message_id, std::string timestamp) {
        if (!connected_ || client_ == nullptr) {
            return false;
        }

        const auto status = device_status_->Snapshot();
        const auto encoded = processor_.EncodeHeartbeat(std::move(message_id), timestamp, status.current_state,
                                                        status.uptime, status.job_id, status.error_code);
        const std::string topic = mqtt::DeviceHeartbeatTopic(config_.device_id);
        const bool published = PublishEncoded(topic, encoded, 0, false, "heartbeat");
        if (published) {
            device_status_->MarkCommunication(std::move(timestamp));
        }
        return published;
    }

    [[nodiscard]] bool PublishEvent(const mqtt::MqttMessage& message) {
        if (!connected_ || client_ == nullptr) {
            return false;
        }
        return PublishEncoded(mqtt::DeviceEventTopic(config_.device_id), processor_.EncodeDeviceEvent(message), 1,
                              false, "device event");
    }

    [[nodiscard]] bool PublishError(const mqtt::MqttMessage& message) {
        if (!connected_ || client_ == nullptr) {
            return false;
        }
        return PublishEncoded(mqtt::DeviceErrorTopic(config_.device_id), processor_.EncodeDeviceError(message), 1, true,
                              "device error");
    }

private:
    [[nodiscard]] std::string LifecycleMessageId(std::string_view kind, std::uint64_t sequence) const {
        return std::string(kind) + '-' + MakeMessageId(config_.device_id, message_session_id_, sequence);
    }

    [[nodiscard]] bool PublishEncoded(const std::string& topic, const mqtt::EncodeResult& encoded, int qos, bool retain,
                                      std::string_view description) {
        if (!encoded.IsSuccess() || encoded.payload.size() > static_cast<std::size_t>(INT_MAX)) {
            std::cerr << "[device][mqtt][ERROR] unable to encode " << description;
            if (!encoded.status.message.empty()) {
                std::cerr << ": " << encoded.status.message;
            }
            std::cerr << '\n';
            return false;
        }

        const int result = mosquitto_publish(client_, nullptr, topic.c_str(), static_cast<int>(encoded.payload.size()),
                                             encoded.payload.data(), qos, retain);
        if (result != MOSQ_ERR_SUCCESS) {
            std::cerr << "[device][mqtt][ERROR] " << description << " publish failed: " << ErrorText(result) << '\n';
            return false;
        }
        return true;
    }

    [[nodiscard]] bool PublishOnlineStatusAndRegistration(std::string_view timestamp) {
        const auto generation = ++connection_generation_;
        const auto status = device_status_->Snapshot();

        const auto online = processor_.EncodeOnlineStatus(LifecycleMessageId("STATUS", generation),
                                                          std::string(timestamp), status.current_state);
        const bool online_published =
            PublishEncoded(mqtt::DeviceStatusTopic(config_.device_id), online, 1, true, "online status");

        const auto registration = processor_.EncodeDeviceRegistration(
            LifecycleMessageId("REGISTER", generation), std::string(timestamp), device_type_, config_.node_name,
            config_.ip_address, status.uart_connected);
        const bool registration_published =
            PublishEncoded(mqtt::DeviceRegisterTopic(config_.device_id), registration, 1, false, "device registration");
        return online_published && registration_published;
    }

    [[nodiscard]] int ConfigureLastWill() {
        const auto encoded = processor_.EncodeOfflineStatus(LifecycleMessageId("WILL", 0), CurrentIso8601Timestamp());
        if (!encoded.IsSuccess() || encoded.payload.size() > static_cast<std::size_t>(INT_MAX)) {
            return MOSQ_ERR_INVAL;
        }

        const std::string topic = mqtt::DeviceStatusTopic(config_.device_id);
        return mosquitto_will_set(client_, topic.c_str(), static_cast<int>(encoded.payload.size()),
                                  encoded.payload.data(), 1, true);
    }

    void HandleConnected(int reason_code) {
        if (reason_code != 0) {
            connected_ = false;
            std::cerr << "[device][mqtt][ERROR] broker rejected connection: " << ConnectionReasonText(reason_code)
                      << '\n';
            return;
        }

        connected_ = true;
        const std::string timestamp = CurrentIso8601Timestamp();
        device_status_->SetConnectionState(mqtt::ConnectionState::kOnline);
        device_status_->MarkCommunication(timestamp);
        const std::string command_topic = mqtt::DeviceCommandTopic(config_.device_id);
        const std::string broadcast_topic(mqtt::kSystemBroadcastCommandTopic);
        const int command_result = mosquitto_subscribe(client_, nullptr, command_topic.c_str(), 1);
        const int broadcast_result = mosquitto_subscribe(client_, nullptr, broadcast_topic.c_str(), 1);
        const bool lifecycle_published = PublishOnlineStatusAndRegistration(timestamp);
        if (command_result != MOSQ_ERR_SUCCESS || broadcast_result != MOSQ_ERR_SUCCESS) {
            std::cerr << "[device][mqtt][ERROR] MQTT command subscription failed\n";
        }
        if (command_result == MOSQ_ERR_SUCCESS && broadcast_result == MOSQ_ERR_SUCCESS && lifecycle_published) {
            std::clog << "[device][mqtt][INFO] MQTT broker connected; command topics subscribed; online status and "
                         "registration published\n";
        }
    }

    void HandleDisconnected(int reason_code) {
        connected_ = false;
        const std::string timestamp = CurrentIso8601Timestamp();
        device_status_->SetConnectionState(running_ ? mqtt::ConnectionState::kReconnecting
                                                    : mqtt::ConnectionState::kOffline);
        device_status_->MarkCommunication(timestamp);
        if (running_) {
            std::cerr << "[device][mqtt][WARN] MQTT broker disconnected (" << reason_code
                      << "); automatic reconnect is active\n";
        }
    }

    void HandleMessage(const mosquitto_message& received) {
        if (received.topic == nullptr || received.payloadlen < 0) {
            return;
        }
        device_status_->MarkCommunication(CurrentIso8601Timestamp());
        const char* bytes = received.payload == nullptr ? "" : static_cast<const char*>(received.payload);
        const auto decoded = processor_.DecodeCommand(
            received.topic, std::string_view(bytes, static_cast<std::size_t>(received.payloadlen)));
        if (!decoded.IsSuccess()) {
            std::cerr << "[device][mqtt][ERROR] " << decoded.error << '\n';
            return;
        }
        if (decoded.duplicate) {
            std::clog << "[device][mqtt][INFO] duplicate MQTT command ignored: " << decoded.message.message_id << '\n';
            return;
        }

        CommandHandler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = command_handler_;
        }
        if (handler) {
            handler(decoded.message);
        }
    }

    static void OnConnected(mosquitto*, void* object, int reason_code) {
        static_cast<Impl*>(object)->HandleConnected(reason_code);
    }

    static void OnDisconnected(mosquitto*, void* object, int reason_code) {
        static_cast<Impl*>(object)->HandleDisconnected(reason_code);
    }

    static void OnMessage(mosquitto*, void* object, const mosquitto_message* message) {
        if (message != nullptr) {
            static_cast<Impl*>(object)->HandleMessage(*message);
        }
    }

    static void OnLog(mosquitto*, void*, int level, const char* message) {
        if ((level & MOSQ_LOG_ERR) != 0 && message != nullptr) {
            std::cerr << "[device][mqtt][ERROR] " << message << '\n';
        }
    }

    MqttNodeConfig config_;
    std::string device_type_;
    MqttMessageProcessor processor_;
    std::string message_session_id_;
    std::shared_ptr<DeviceStatus> device_status_;
    mosquitto* client_{ nullptr };
    std::atomic_bool running_{ false };
    std::atomic_bool connected_{ false };
    bool loop_started_{ false };
    std::uint64_t connection_generation_{};
    std::mutex handler_mutex_;
    CommandHandler command_handler_;
};

MqttNodeClient::MqttNodeClient(MqttNodeConfig config, std::string device_type,
                               std::shared_ptr<DeviceStatus> device_status)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(device_type), std::move(device_status))) {}

MqttNodeClient::~MqttNodeClient() = default;

void MqttNodeClient::SetCommandHandler(CommandHandler handler) {
    impl_->SetCommandHandler(std::move(handler));
}

bool MqttNodeClient::Start() {
    return impl_->Start();
}

void MqttNodeClient::Stop() noexcept {
    impl_->Stop();
}

bool MqttNodeClient::IsConnected() const noexcept {
    return impl_->IsConnected();
}

bool MqttNodeClient::PublishHeartbeat(std::string message_id, std::string timestamp) {
    return impl_->PublishHeartbeat(std::move(message_id), std::move(timestamp));
}

bool MqttNodeClient::PublishEvent(const contracts::mqtt::MqttMessage& message) {
    return impl_->PublishEvent(message);
}

bool MqttNodeClient::PublishError(const contracts::mqtt::MqttMessage& message) {
    return impl_->PublishError(message);
}

}  // namespace logistics::device
