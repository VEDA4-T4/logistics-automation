#include "logistics/central_server/mqtt_client.hpp"

#include <array>
#include <iostream>
#include <string>
#include <utility>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;

struct Subscription final {
    std::string_view topic_filter;
    int qos;
};

inline constexpr std::array kRequiredSubscriptions{
    Subscription{ mqtt::kServerRequestSubscription, 1 },   Subscription{ mqtt::kDeviceRegisterSubscription, 1 },
    Subscription{ mqtt::kDeviceResponseSubscription, 1 },  Subscription{ mqtt::kDeviceStatusSubscription, 1 },
    Subscription{ mqtt::kDeviceEventSubscription, 1 },     Subscription{ mqtt::kDeviceErrorSubscription, 1 },
    Subscription{ mqtt::kDeviceHeartbeatSubscription, 1 },
};

[[nodiscard]] bool IsValidPublishTopic(std::string_view topic) noexcept {
    return !topic.empty() && topic.find('+') == std::string_view::npos && topic.find('#') == std::string_view::npos &&
           topic.find('\0') == std::string_view::npos && mqtt::ParseTopic(topic).IsValid();
}

[[nodiscard]] constexpr bool IsQosAllowed(const mqtt::DeliveryPolicy& policy, mqtt::Qos qos) noexcept {
    const auto value = static_cast<std::uint8_t>(qos);
    return value >= static_cast<std::uint8_t>(policy.minimum_qos) &&
           value <= static_cast<std::uint8_t>(policy.maximum_qos);
}

[[nodiscard]] constexpr bool IsRetainAllowed(const mqtt::DeliveryPolicy& policy, bool retain) noexcept {
    return !retain || policy.retain != mqtt::RetainPolicy::kNever;
}

void DefaultLog(MqttLogLevel level, std::string_view message) {
    const char* label = "INFO";
    std::ostream* output = &std::clog;
    if (level == MqttLogLevel::kWarning) {
        label = "WARN";
        output = &std::cerr;
    } else if (level == MqttLogLevel::kError) {
        label = "ERROR";
        output = &std::cerr;
    }
    *output << "[mqtt][" << label << "] " << message << '\n';
}

}  // namespace

MqttClient::MqttClient(MqttConfig config, std::unique_ptr<MqttTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)), logger_(DefaultLog) {}

MqttClient::~MqttClient() {
    Stop();
}

void MqttClient::SetLogger(Logger logger) {
    std::lock_guard lock(callback_mutex_);
    logger_ = logger ? std::move(logger) : Logger(DefaultLog);
}

void MqttClient::SetMessageHandler(MessageHandler handler) {
    std::lock_guard lock(callback_mutex_);
    message_handler_ = std::move(handler);
}

bool MqttClient::Start() {
    if (running_.exchange(true)) {
        return true;
    }
    if (!transport_) {
        running_ = false;
        Log(MqttLogLevel::kError, "MQTT transport is not available");
        return false;
    }
    if (!config_.IsValid()) {
        running_ = false;
        Log(MqttLogLevel::kError, "MQTT configuration is invalid");
        return false;
    }

    stopping_ = false;
    transport_->SetCallbacks({
        .connected = [this](int code, std::string_view reason) { HandleConnected(code, reason); },
        .disconnected = [this](int code, std::string_view reason) { HandleDisconnected(code, reason); },
        .message_received = [this](std::string_view topic, std::string_view payload) { HandleMessage(topic, payload); },
        .log_received = [this](MqttTransportLogLevel level,
                               std::string_view message) { HandleTransportLog(level, message); },
    });

    const MqttTransportOptions options{
        .host = config_.host,
        .client_id = config_.client_id,
        .username = config_.username,
        .password = config_.password,
        .port = config_.port,
        .keep_alive_seconds = config_.keep_alive_seconds,
        .reconnect_min_delay_seconds = config_.reconnect_min_delay_seconds,
        .reconnect_max_delay_seconds = config_.reconnect_max_delay_seconds,
        .clean_session = config_.clean_session,
    };
    const auto result = transport_->Start(options);
    if (!result) {
        connected_ = false;
        running_ = false;
        Log(MqttLogLevel::kError, "unable to start MQTT connection: " + result.message);
        return false;
    }

    Log(MqttLogLevel::kInfo, "MQTT connection started for " + config_.host + ":" + std::to_string(config_.port));
    return true;
}

void MqttClient::Stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }
    stopping_ = true;
    connected_ = false;
    if (transport_) {
        transport_->Stop();
    }
    Log(MqttLogLevel::kInfo, "MQTT connection stopped");
}

bool MqttClient::IsConnected() const noexcept {
    return connected_;
}

bool MqttClient::Publish(std::string_view topic, std::string_view payload, int qos, bool retain) {
    if (!IsValidPublishTopic(topic) || qos < 0 || qos > 2) {
        Log(MqttLogLevel::kError, "rejected MQTT publish with invalid topic or QoS");
        return false;
    }
    if (!connected_) {
        Log(MqttLogLevel::kWarning, "MQTT publish rejected while disconnected");
        return false;
    }

    const auto result = transport_->Publish(topic, payload, qos, retain);
    if (!result) {
        Log(MqttLogLevel::kError, "MQTT publish failed for " + std::string(topic) + ": " + result.message);
        return false;
    }
    return true;
}

bool MqttClient::PublishMessage(std::string_view topic, const mqtt::MqttMessage& message, mqtt::Qos qos, bool retain) {
    const auto validation = mqtt::ValidateTopicMessage(topic, message);
    if (!validation.IsSuccess()) {
        Log(MqttLogLevel::kError,
            "rejected MQTT message publish; error=" + std::string(mqtt::ToString(validation.error)) +
                "; message=" + validation.message);
        return false;
    }

    const auto policy = mqtt::PolicyFor(message.message_type);
    if (!IsQosAllowed(policy, qos) || !IsRetainAllowed(policy, retain)) {
        Log(MqttLogLevel::kError, "rejected MQTT message publish that violates QoS or retain policy");
        return false;
    }

    const auto encoded = mqtt::SerializeMessage(message);
    if (!encoded.IsSuccess()) {
        Log(MqttLogLevel::kError,
            "MQTT serialization failed; error=" + std::string(mqtt::ToString(encoded.status.error)) +
                "; field=" + encoded.status.field + "; message=" + encoded.status.message);
        return false;
    }

    return Publish(topic, encoded.payload, static_cast<int>(qos), retain);
}

void MqttClient::HandleConnected(int reason_code, std::string_view reason) {
    if (stopping_ || !running_) {
        return;
    }
    if (reason_code != 0) {
        connected_ = false;
        Log(MqttLogLevel::kError,
            "MQTT broker rejected the connection (" + std::to_string(reason_code) + "): " + std::string(reason));
        return;
    }

    connected_ = true;
    Log(MqttLogLevel::kInfo, "MQTT broker connected");
    SubscribeRequiredTopics();
}

void MqttClient::HandleDisconnected(int reason_code, std::string_view reason) {
    connected_ = false;
    if (stopping_) {
        return;
    }

    Log(MqttLogLevel::kWarning, "MQTT broker disconnected (" + std::to_string(reason_code) +
                                    "): " + std::string(reason) + "; automatic reconnect is active");
}

void MqttClient::HandleMessage(std::string_view topic, std::string_view payload) {
    MessageHandler handler;
    {
        std::lock_guard lock(callback_mutex_);
        handler = message_handler_;
    }
    if (handler) {
        handler(topic, payload);
    }
}

void MqttClient::HandleTransportLog(MqttTransportLogLevel level, std::string_view message) {
    switch (level) {
        case MqttTransportLogLevel::kError:
            Log(MqttLogLevel::kError, message);
            break;
        case MqttTransportLogLevel::kWarning:
            Log(MqttLogLevel::kWarning, message);
            break;
        case MqttTransportLogLevel::kInfo:
            Log(MqttLogLevel::kInfo, message);
            break;
        case MqttTransportLogLevel::kDebug:
            break;
    }
}

void MqttClient::SubscribeRequiredTopics() {
    for (const auto& subscription : kRequiredSubscriptions) {
        const auto result = transport_->Subscribe(subscription.topic_filter, subscription.qos);
        if (!result) {
            Log(MqttLogLevel::kError,
                "MQTT subscription failed for " + std::string(subscription.topic_filter) + ": " + result.message);
        }
    }
}

void MqttClient::Log(MqttLogLevel level, std::string_view message) const {
    Logger logger;
    {
        std::lock_guard lock(callback_mutex_);
        logger = logger_;
    }
    if (logger) {
        logger(level, message);
    }
}

}  // namespace logistics::central_server
