#include <mosquitto.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "logistics/central_server/mqtt_transport.hpp"

namespace logistics::central_server {
namespace {

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

[[nodiscard]] MqttOperationResult MakeResult(int code, int packet_id = 0) {
    const char* description = code == MOSQ_ERR_SUCCESS ? nullptr : mosquitto_strerror(code);
    return {
        .code = code,
        .message = description == nullptr ? std::string{} : std::string(description),
        .packet_id = packet_id,
    };
}

[[nodiscard]] std::string_view ConnectionReason(int reason_code) noexcept {
    const char* reason = mosquitto_connack_string(reason_code);
    return reason == nullptr ? std::string_view("unknown connection result") : std::string_view(reason);
}

[[nodiscard]] MqttTransportLogLevel MapLogLevel(int level) noexcept {
    if ((level & MOSQ_LOG_ERR) != 0) {
        return MqttTransportLogLevel::kError;
    }
    if ((level & MOSQ_LOG_WARNING) != 0) {
        return MqttTransportLogLevel::kWarning;
    }
    if ((level & MOSQ_LOG_NOTICE) != 0 || (level & MOSQ_LOG_INFO) != 0) {
        return MqttTransportLogLevel::kInfo;
    }
    return MqttTransportLogLevel::kDebug;
}

class MosquittoTransport final : public MqttTransport {
public:
    ~MosquittoTransport() override {
        Stop();
    }

    void SetCallbacks(MqttTransportCallbacks callbacks) override {
        callbacks_ = std::move(callbacks);
    }

    [[nodiscard]] MqttOperationResult Start(const MqttTransportOptions& options) override {
        Stop();

        const int library_result = Library().Result();
        if (library_result != MOSQ_ERR_SUCCESS) {
            return MakeResult(library_result);
        }

        client_ = mosquitto_new(options.client_id.c_str(), options.clean_session, this);
        if (client_ == nullptr) {
            return {
                .code = MOSQ_ERR_NOMEM,
                .message = "unable to allocate Mosquitto client",
            };
        }

        mosquitto_connect_callback_set(client_, &MosquittoTransport::OnConnected);
        mosquitto_disconnect_callback_set(client_, &MosquittoTransport::OnDisconnected);
        mosquitto_message_callback_set(client_, &MosquittoTransport::OnMessage);
        mosquitto_publish_callback_set(client_, &MosquittoTransport::OnPublish);
        mosquitto_subscribe_callback_set(client_, &MosquittoTransport::OnSubscribe);
        mosquitto_log_callback_set(client_, &MosquittoTransport::OnLog);

        int result = mosquitto_int_option(client_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
        if (result == MOSQ_ERR_SUCCESS && !options.username.empty()) {
            result = mosquitto_username_pw_set(client_, options.username.c_str(),
                                               options.password.empty() ? nullptr : options.password.c_str());
        }
        if (result == MOSQ_ERR_SUCCESS && options.tls_enabled) {
            if (options.ca_certificate.empty()) {
                result = MOSQ_ERR_INVAL;
            } else {
                result = mosquitto_tls_set(client_, options.ca_certificate.c_str(), nullptr, nullptr, nullptr, nullptr);
            }
        }
        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_reconnect_delay_set(client_, options.reconnect_min_delay_seconds,
                                                   options.reconnect_max_delay_seconds, true);
        }
        if (result == MOSQ_ERR_SUCCESS && options.will.has_value()) {
            if (options.will->payload.size() > static_cast<std::size_t>(INT_MAX)) {
                result = MOSQ_ERR_PAYLOAD_SIZE;
            } else {
                result = mosquitto_will_set(client_, options.will->topic.c_str(),
                                            static_cast<int>(options.will->payload.size()),
                                            options.will->payload.data(), options.will->qos, options.will->retain);
            }
        }
        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_connect_async(client_, options.host.c_str(), options.port, options.keep_alive_seconds);
        }
        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_loop_start(client_);
            loop_started_ = result == MOSQ_ERR_SUCCESS;
        }

        if (result != MOSQ_ERR_SUCCESS) {
            const auto error = MakeResult(result);
            Stop();
            return error;
        }
        return MakeResult(MOSQ_ERR_SUCCESS);
    }

    void Stop(bool graceful = true) noexcept override {
        if (client_ == nullptr) {
            return;
        }

        reconnect_requested_ = false;
        stopping_ = true;
        if (graceful) {
            static_cast<void>(mosquitto_disconnect(client_));
        }
        if (loop_started_) {
            static_cast<void>(mosquitto_loop_stop(client_, !graceful));
            loop_started_ = false;
        }
        mosquitto_destroy(client_);
        client_ = nullptr;
    }

    [[nodiscard]] MqttOperationResult RequestReconnect() override {
        if (client_ == nullptr) {
            return {
                .code = MOSQ_ERR_NO_CONN,
                .message = "Mosquitto client is not running",
            };
        }
        if (!connected_.load()) {
            return MakeResult(mosquitto_reconnect_async(client_));
        }
        reconnect_requested_ = true;
        const auto result = MakeResult(mosquitto_disconnect(client_));
        if (!result) {
            reconnect_requested_ = false;
        }
        return result;
    }

    [[nodiscard]] MqttOperationResult Publish(std::string_view topic, std::string_view payload, int qos,
                                              bool retain) override {
        if (client_ == nullptr) {
            return {
                .code = MOSQ_ERR_NO_CONN,
                .message = "Mosquitto client is not running",
            };
        }
        if (payload.size() > static_cast<std::size_t>(INT_MAX)) {
            return {
                .code = MOSQ_ERR_INVAL,
                .message = "MQTT payload is too large",
            };
        }

        const std::string topic_text(topic);
        int packet_id = 0;
        const int result = mosquitto_publish(client_, &packet_id, topic_text.c_str(), static_cast<int>(payload.size()),
                                             payload.data(), qos, retain);
        return MakeResult(result, packet_id);
    }

    [[nodiscard]] MqttOperationResult Subscribe(std::string_view topic_filter, int qos) override {
        if (client_ == nullptr) {
            return {
                .code = MOSQ_ERR_NO_CONN,
                .message = "Mosquitto client is not running",
            };
        }

        const std::string topic_text(topic_filter);
        int packet_id = 0;
        return MakeResult(mosquitto_subscribe(client_, &packet_id, topic_text.c_str(), qos), packet_id);
    }

private:
    static void OnConnected(mosquitto*, void* object, int reason_code) {
        auto* self = static_cast<MosquittoTransport*>(object);
        if (self != nullptr) {
            self->connected_ = reason_code == 0;
            self->stopping_ = false;
        }
        if (self != nullptr && self->callbacks_.connected) {
            self->callbacks_.connected(reason_code, ConnectionReason(reason_code));
        }
    }

    static void OnDisconnected(mosquitto*, void* object, int reason_code) {
        auto* self = static_cast<MosquittoTransport*>(object);
        if (self == nullptr) {
            return;
        }
        self->connected_ = false;
        if (self != nullptr && self->callbacks_.disconnected) {
            const char* reason = mosquitto_strerror(reason_code);
            self->callbacks_.disconnected(reason_code, reason == nullptr ? std::string_view("unknown disconnect reason")
                                                                         : std::string_view(reason));
        }
    }

    static void OnMessage(mosquitto*, void* object, const mosquitto_message* message) {
        auto* self = static_cast<MosquittoTransport*>(object);
        if (self == nullptr || message == nullptr || message->topic == nullptr || !self->callbacks_.message_received) {
            return;
        }

        const auto* payload = static_cast<const char*>(message->payload);
        const std::string_view payload_view =
            payload == nullptr || message->payloadlen <= 0
                ? std::string_view{}
                : std::string_view(payload, static_cast<std::size_t>(message->payloadlen));
        self->callbacks_.message_received(message->topic, payload_view, message->qos, message->retain != 0);
    }

    static void OnPublish(mosquitto*, void* object, int packet_id) {
        auto* self = static_cast<MosquittoTransport*>(object);
        if (self != nullptr && self->callbacks_.publish_acknowledged) {
            self->callbacks_.publish_acknowledged(packet_id);
        }
        if (self->reconnect_requested_.exchange(false) && !self->stopping_.load() && self->client_ != nullptr) {
            const int result = mosquitto_reconnect_async(self->client_);
            if (result != MOSQ_ERR_SUCCESS && self->callbacks_.log_received) {
                self->callbacks_.log_received(MqttTransportLogLevel::kError,
                                              "Mosquitto reconnect after forced disconnect failed");
            }
        }
    }

    static void OnSubscribe(mosquitto*, void* object, int packet_id, int qos_count, const int* granted_qos) {
        auto* self = static_cast<MosquittoTransport*>(object);
        if (self == nullptr || !self->callbacks_.subscribe_acknowledged || qos_count < 0) {
            return;
        }
        std::vector<int> granted;
        if (granted_qos != nullptr) {
            granted.assign(granted_qos, granted_qos + qos_count);
        }
        self->callbacks_.subscribe_acknowledged(packet_id, granted);
    }

    static void OnLog(mosquitto*, void* object, int level, const char* message) {
        auto* self = static_cast<MosquittoTransport*>(object);
        if (self != nullptr && message != nullptr && self->callbacks_.log_received) {
            self->callbacks_.log_received(MapLogLevel(level), message);
        }
    }

    MqttTransportCallbacks callbacks_;
    mosquitto* client_{ nullptr };
    bool loop_started_{ false };
    std::atomic_bool connected_{ false };
    std::atomic_bool reconnect_requested_{ false };
    std::atomic_bool stopping_{ false };
};

}  // namespace

std::unique_ptr<MqttTransport> CreateMosquittoTransport() {
    return std::make_unique<MosquittoTransport>();
}

}  // namespace logistics::central_server
