#include "logistics/device/mqtt_node_client.hpp"

#include <mosquitto.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <iostream>
#include <mutex>
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
    explicit Impl(MqttNodeConfig config) : config_(std::move(config)), processor_(config_.device_id) {}

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
        if (!config_.IsValid()) {
            running_ = false;
            std::cerr << "[device][mqtt][ERROR] invalid MQTT node configuration\n";
            return false;
        }
        if (Library().Result() != MOSQ_ERR_SUCCESS) {
            running_ = false;
            std::cerr << "[device][mqtt][ERROR] unable to initialize libmosquitto: " << ErrorText(Library().Result())
                      << '\n';
            return false;
        }

        client_ = mosquitto_new(config_.client_id.c_str(), config_.clean_session, this);
        if (client_ == nullptr) {
            running_ = false;
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
            return;
        }
        connected_ = false;
        if (client_ == nullptr) {
            return;
        }

        static_cast<void>(mosquitto_disconnect(client_));
        if (loop_started_) {
            static_cast<void>(mosquitto_loop_stop(client_, true));
            loop_started_ = false;
        }
        mosquitto_destroy(client_);
        client_ = nullptr;
    }

    [[nodiscard]] bool IsConnected() const noexcept {
        return connected_;
    }

    [[nodiscard]] bool PublishHeartbeat(std::string message_id, std::string timestamp, std::string current_state,
                                        std::uint64_t uptime) {
        if (!connected_ || client_ == nullptr) {
            return false;
        }

        const auto encoded =
            processor_.EncodeHeartbeat(std::move(message_id), std::move(timestamp), std::move(current_state), uptime);
        if (!encoded.IsSuccess() || encoded.payload.size() > static_cast<std::size_t>(INT_MAX)) {
            std::cerr << "[device][mqtt][ERROR] unable to encode heartbeat\n";
            return false;
        }

        const std::string topic = mqtt::DeviceHeartbeatTopic(config_.device_id);
        const int result = mosquitto_publish(client_, nullptr, topic.c_str(), static_cast<int>(encoded.payload.size()),
                                             encoded.payload.data(), 0, false);
        if (result != MOSQ_ERR_SUCCESS) {
            std::cerr << "[device][mqtt][ERROR] heartbeat publish failed: " << ErrorText(result) << '\n';
            return false;
        }
        return true;
    }

private:
    [[nodiscard]] int ConfigureLastWill() {
        const auto encoded = processor_.EncodeOfflineStatus("WILL-" + config_.device_id, CurrentIso8601Timestamp());
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
        const std::string command_topic = mqtt::DeviceCommandTopic(config_.device_id);
        const std::string broadcast_topic(mqtt::kSystemBroadcastCommandTopic);
        const int command_result = mosquitto_subscribe(client_, nullptr, command_topic.c_str(), 1);
        const int broadcast_result = mosquitto_subscribe(client_, nullptr, broadcast_topic.c_str(), 1);
        if (command_result != MOSQ_ERR_SUCCESS || broadcast_result != MOSQ_ERR_SUCCESS) {
            std::cerr << "[device][mqtt][ERROR] MQTT command subscription failed\n";
        } else {
            std::clog << "[device][mqtt][INFO] MQTT broker connected and command topics subscribed\n";
        }
    }

    void HandleDisconnected(int reason_code) {
        connected_ = false;
        if (running_) {
            std::cerr << "[device][mqtt][WARN] MQTT broker disconnected (" << reason_code
                      << "); automatic reconnect is active\n";
        }
    }

    void HandleMessage(const mosquitto_message& received) {
        if (received.topic == nullptr || received.payloadlen < 0) {
            return;
        }
        const char* bytes = received.payload == nullptr ? "" : static_cast<const char*>(received.payload);
        const auto decoded = processor_.DecodeCommand(
            received.topic, std::string_view(bytes, static_cast<std::size_t>(received.payloadlen)));
        if (!decoded.IsSuccess()) {
            std::cerr << "[device][mqtt][ERROR] " << decoded.error << '\n';
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
    MqttMessageProcessor processor_;
    mosquitto* client_{ nullptr };
    std::atomic_bool running_{ false };
    std::atomic_bool connected_{ false };
    bool loop_started_{ false };
    std::mutex handler_mutex_;
    CommandHandler command_handler_;
};

MqttNodeClient::MqttNodeClient(MqttNodeConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

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

bool MqttNodeClient::PublishHeartbeat(std::string message_id, std::string timestamp, std::string current_state,
                                      std::uint64_t uptime) {
    return impl_->PublishHeartbeat(std::move(message_id), std::move(timestamp), std::move(current_state), uptime);
}

}  // namespace logistics::device
