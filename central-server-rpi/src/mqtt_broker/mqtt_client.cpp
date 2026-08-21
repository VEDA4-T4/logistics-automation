#include "logistics/central_server/mqtt_client.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
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

inline constexpr std::size_t kMaximumMqttPayloadSize = 268435455U;

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

[[nodiscard]] std::string PublishContext(std::string_view topic, const mqtt::MqttMessage& message,
                                         const int packet_id) {
    std::ostringstream output;
    output << "MQTT publish accepted; topic=" << topic << "; messageId=" << message.message_id
           << "; messageType=" << mqtt::ToString(message.message_type) << "; source=" << message.source_id
           << "; packetId=" << packet_id;
    if (const auto* command = mqtt::GetPayload<mqtt::ControlCommandPayload>(message); command != nullptr) {
        output << "; requestId=" << command->request_id << "; command=" << mqtt::ToString(command->command)
               << "; target=" << command->target_device_id << "; component=" << command->component_id;
        if (command->params.is_object() && command->params.contains("workId") &&
            command->params.at("workId").is_string()) {
            output << "; workId=" << command->params.at("workId").get<std::string>();
        }
    } else if (const auto* emergency = mqtt::GetPayload<mqtt::EmergencyStopPayload>(message); emergency != nullptr) {
        output << "; requestId=" << emergency->request_id << "; command=" << mqtt::ToString(emergency->command)
               << "; target=" << emergency->target_device_id;
    } else if (const auto* destination = mqtt::GetPayload<mqtt::DestinationSetPayload>(message);
               destination != nullptr) {
        output << "; requestId=" << destination->request_id << "; command=DESTINATION_SET"
               << "; target=" << destination->target_device_id << "; workId=" << destination->work_id
               << "; destination=" << destination->destination;
    } else if (const auto* response = mqtt::GetPayload<mqtt::CommandResponsePayload>(message); response != nullptr) {
        output << "; requestId=" << response->request_id << "; command=" << mqtt::ToString(response->command)
               << "; result=" << mqtt::ToString(response->result);
    } else if (const auto* work = mqtt::GetPayload<mqtt::WorkCreatedPayload>(message); work != nullptr) {
        output << "; workId=" << work->work_id;
    }
    return output.str();
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

[[nodiscard]] std::string CurrentIso8601Timestamp() {
    const auto current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
#ifdef _WIN32
    gmtime_s(&utc_time, &current_time);
#else
    gmtime_r(&current_time, &utc_time);
#endif
    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
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

void MqttClient::SetDisconnectedHandler(DisconnectedHandler handler) {
    std::lock_guard lock(callback_mutex_);
    disconnected_handler_ = std::move(handler);
}

bool MqttClient::SetWill(MqttWill will) {
    if (running_) {
        Log(MqttLogLevel::kError, "MQTT Last Will cannot be changed while the client is running");
        return false;
    }
    if (!IsValidPublishTopic(will.topic) || will.qos < 0 || will.qos > 2 ||
        will.payload.size() > kMaximumMqttPayloadSize) {
        Log(MqttLogLevel::kError, "rejected invalid MQTT Last Will configuration");
        return false;
    }

    will_ = std::move(will);
    custom_will_ = true;
    return true;
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
    started_at_ = std::chrono::steady_clock::now();
    session_id_ =
        std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(started_at_.time_since_epoch()).count());
    transport_->SetCallbacks({
        .connected = [this](int code, std::string_view reason) { HandleConnected(code, reason); },
        .disconnected = [this](int code, std::string_view reason) { HandleDisconnected(code, reason); },
        .message_received = [this](std::string_view topic, std::string_view payload, int qos,
                                   bool retained) { HandleMessage(topic, payload, qos, retained); },
        .publish_acknowledged = [this](int packet_id) { HandlePublishAcknowledged(packet_id); },
        .subscribe_acknowledged =
            [this](int packet_id, const std::vector<int>& granted_qos) {
                HandleSubscribeAcknowledged(packet_id, granted_qos);
            },
        .log_received = [this](MqttTransportLogLevel level,
                               std::string_view message) { HandleTransportLog(level, message); },
    });

    if (!custom_will_) {
        const auto offline_status = MakeServerStatusMessage(mqtt::ConnectionState::kOffline);
        const auto encoded = mqtt::SerializeMessage(offline_status);
        if (!encoded.IsSuccess()) {
            running_ = false;
            Log(MqttLogLevel::kError, "unable to serialize MQTT Last Will status");
            return false;
        }
        will_ = MqttWill{
            .topic = std::string(mqtt::kServerStatusTopic),
            .payload = encoded.payload,
            .qos = 1,
            .retain = true,
        };
    }

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
        .tls_enabled = config_.tls_enabled,
        .ca_certificate = config_.ca_certificate,
        .will = will_,
    };
    const auto result = transport_->Start(options);
    if (!result) {
        connected_ = false;
        running_ = false;
        Log(MqttLogLevel::kError, "unable to start MQTT connection: " + result.message);
        return false;
    }

    StartHeartbeatLoop();
    Log(MqttLogLevel::kInfo, "MQTT connection started for " + config_.host + ":" + std::to_string(config_.port));
    return true;
}

void MqttClient::Stop() noexcept {
    if (!running_.exchange(false)) {
        return;
    }
    stopping_ = true;
    heartbeat_thread_.request_stop();
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }

    bool graceful = true;
    if (connected_) {
        struct OfflineAcknowledgement final {
            std::mutex mutex;
            std::condition_variable received;
            bool acknowledged{};
        };
        const auto offline_ack = std::make_shared<OfflineAcknowledgement>();
        const auto offline = MakeServerStatusMessage(mqtt::ConnectionState::kOffline);
        const bool accepted = PublishMessageAcknowledged(
            mqtt::kServerStatusTopic, offline, mqtt::Qos::kAtLeastOnce,
            [offline_ack](int) {
                {
                    const std::lock_guard lock(offline_ack->mutex);
                    offline_ack->acknowledged = true;
                }
                offline_ack->received.notify_one();
            },
            true);
        std::unique_lock lock(offline_ack->mutex);
        graceful = accepted && offline_ack->received.wait_for(lock, std::chrono::milliseconds(500),
                                                              [&offline_ack] { return offline_ack->acknowledged; });
        if (!graceful) {
            Log(MqttLogLevel::kWarning,
                "retained OFFLINE status was not acknowledged; closing ungracefully so the broker publishes LWT");
        }
    }
    connected_ = false;
    ready_ = false;
    if (transport_) {
        transport_->Stop(graceful);
    }
    {
        const std::lock_guard lock(callback_mutex_);
        pending_publish_acknowledgements_.clear();
        early_publish_acknowledgements_.clear();
        publish_registration_in_progress_ = false;
    }
    Log(MqttLogLevel::kInfo, "MQTT connection stopped");
}

bool MqttClient::IsConnected() const noexcept {
    return connected_;
}

bool MqttClient::IsReady() const noexcept {
    return ready_;
}

bool MqttClient::Publish(std::string_view topic, std::string_view payload, int qos, bool retain) {
    return PublishWithReceipt(topic, payload, qos, retain).has_value();
}

std::optional<int> MqttClient::PublishWithReceipt(std::string_view topic, std::string_view payload, int qos,
                                                  bool retain) {
    if (!IsValidPublishTopic(topic) || qos < 0 || qos > 2) {
        Log(MqttLogLevel::kError, "rejected MQTT publish with invalid topic or QoS");
        return std::nullopt;
    }
    if (!connected_) {
        Log(MqttLogLevel::kWarning, "MQTT publish rejected while disconnected");
        return std::nullopt;
    }

    return PublishWithReceiptUnchecked(topic, payload, qos, retain);
}

std::optional<int> MqttClient::PublishWithReceiptUnchecked(std::string_view topic, std::string_view payload, int qos,
                                                           bool retain) {
    const auto result = transport_->Publish(topic, payload, qos, retain);
    if (!result) {
        Log(MqttLogLevel::kError, "MQTT publish failed for " + std::string(topic) + ": " + result.message);
        return std::nullopt;
    }
    return result.packet_id;
}

bool MqttClient::PublishMessage(std::string_view topic, const mqtt::MqttMessage& message, mqtt::Qos qos, bool retain) {
    return PublishMessageWithReceipt(topic, message, qos, retain).has_value();
}

bool MqttClient::PublishTransientMessage(std::string_view topic, const mqtt::MqttMessage& message) {
    if (!mqtt::IsTransientTelemetry(message.message_type)) {
        return false;
    }
    return PublishMessage(topic, message, mqtt::PolicyFor(message.message_type).minimum_qos, false);
}

std::optional<int> MqttClient::PublishMessageWithReceipt(std::string_view topic, const mqtt::MqttMessage& message,
                                                         mqtt::Qos qos, bool retain) {
    const auto validation = mqtt::ValidateTopicMessage(topic, message);
    if (!validation.IsSuccess()) {
        Log(MqttLogLevel::kError,
            "rejected MQTT message publish; error=" + std::string(mqtt::ToString(validation.error)) +
                "; message=" + validation.message);
        return std::nullopt;
    }

    const auto policy = mqtt::PolicyFor(message.message_type);
    if (!IsQosAllowed(policy, qos) || !IsRetainAllowed(policy, retain)) {
        Log(MqttLogLevel::kError, "rejected MQTT message publish that violates QoS or retain policy");
        return std::nullopt;
    }

    const auto encoded = mqtt::SerializeMessage(message);
    if (!encoded.IsSuccess()) {
        Log(MqttLogLevel::kError,
            "MQTT serialization failed; error=" + std::string(mqtt::ToString(encoded.status.error)) +
                "; field=" + encoded.status.field + "; message=" + encoded.status.message);
        return std::nullopt;
    }

    const auto receipt = PublishWithReceipt(topic, encoded.payload, static_cast<int>(qos), retain);
    if (receipt.has_value() && connected_ && !mqtt::IsTransientTelemetry(message.message_type)) {
        Log(MqttLogLevel::kInfo, PublishContext(topic, message, *receipt));
    }
    return receipt;
}

bool MqttClient::PublishMessageAcknowledged(std::string_view topic, const mqtt::MqttMessage& message, mqtt::Qos qos,
                                            PublishAcknowledgedHandler acknowledged, bool retain) {
    std::lock_guard registration_lock(publish_registration_mutex_);
    {
        std::lock_guard callback_lock(callback_mutex_);
        publish_registration_in_progress_ = true;
        early_publish_acknowledgements_.clear();
    }
    const auto receipt = PublishMessageWithReceipt(topic, message, qos, retain);
    bool acknowledged_early = false;
    {
        std::lock_guard callback_lock(callback_mutex_);
        publish_registration_in_progress_ = false;
        if (receipt.has_value() && connected_) {
            acknowledged_early = early_publish_acknowledgements_.erase(*receipt) != 0;
            if (!acknowledged_early) {
                pending_publish_acknowledgements_.insert_or_assign(*receipt, acknowledged);
            }
        }
        early_publish_acknowledgements_.clear();
    }
    if (acknowledged_early) {
        acknowledged(*receipt);
    }
    return receipt.has_value() && connected_;
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
    ready_ = false;
    subscription_failed_ = false;
    Log(MqttLogLevel::kInfo, "MQTT broker connected");
    SubscribeRequiredTopics();
}

void MqttClient::HandleDisconnected(int reason_code, std::string_view reason) {
    connected_ = false;
    ready_ = false;
    DisconnectedHandler handler;
    {
        std::lock_guard lock(callback_mutex_);
        pending_publish_acknowledgements_.clear();
        early_publish_acknowledgements_.clear();
        publish_registration_in_progress_ = false;
        handler = disconnected_handler_;
    }
    if (handler) {
        handler();
    }
    if (stopping_) {
        return;
    }

    Log(MqttLogLevel::kWarning, "MQTT broker disconnected (" + std::to_string(reason_code) +
                                    "): " + std::string(reason) + "; automatic reconnect is active");
}

void MqttClient::HandleMessage(std::string_view topic, std::string_view payload, int qos, bool retained) {
    MessageHandler handler;
    {
        std::lock_guard lock(callback_mutex_);
        handler = message_handler_;
    }
    if (handler) {
        handler(topic, payload, qos, retained);
    }
}

void MqttClient::HandlePublishAcknowledged(int packet_id) {
    PublishAcknowledgedHandler handler;
    {
        std::lock_guard lock(callback_mutex_);
        const auto pending = pending_publish_acknowledgements_.find(packet_id);
        if (pending != pending_publish_acknowledgements_.end()) {
            handler = std::move(pending->second);
            pending_publish_acknowledgements_.erase(pending);
        } else if (publish_registration_in_progress_) {
            early_publish_acknowledgements_.insert(packet_id);
        }
    }
    if (handler) {
        handler(packet_id);
    }
}

void MqttClient::HandleSubscribeAcknowledged(int packet_id, const std::vector<int>& granted_qos) {
    std::string topic;
    int requested_qos = -1;
    {
        std::lock_guard lock(subscription_mutex_);
        const auto subscription = pending_subscriptions_.find(packet_id);
        if (subscription == pending_subscriptions_.end()) {
            if (subscription_registration_in_progress_) {
                early_subscription_acknowledgements_.insert_or_assign(packet_id, granted_qos);
                return;
            }
            Log(MqttLogLevel::kWarning, "received MQTT SUBACK for an unknown packet id");
            return;
        }
        topic = std::move(subscription->second.first);
        requested_qos = subscription->second.second;
        pending_subscriptions_.erase(subscription);
    }

    const bool accepted = granted_qos.size() == 1 && granted_qos.front() == requested_qos;
    if (!accepted) {
        subscription_failed_ = true;
        Log(MqttLogLevel::kError, "MQTT subscription rejected by broker for " + topic);
        ready_ = false;
        const auto reconnect = transport_->RequestReconnect();
        if (!reconnect) {
            Log(MqttLogLevel::kError, "MQTT reconnect request after SUBACK rejection failed: " + reconnect.message);
        }
        return;
    }

    bool all_acknowledged = false;
    {
        std::lock_guard lock(subscription_mutex_);
        all_acknowledged = subscriptions_registration_complete_ && pending_subscriptions_.empty();
    }
    if (all_acknowledged && !subscription_failed_) {
        ready_ = true;
        Log(MqttLogLevel::kInfo, "MQTT subscriptions acknowledged; client is ready");
        static_cast<void>(PublishServerStatus(mqtt::ConnectionState::kOnline));
        static_cast<void>(PublishHeartbeat());
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
    const std::lock_guard registration_lock(subscription_registration_mutex_);
    {
        std::lock_guard lock(subscription_mutex_);
        pending_subscriptions_.clear();
        early_subscription_acknowledgements_.clear();
        subscriptions_registration_complete_ = false;
    }
    bool failed = false;
    for (const auto& subscription : kRequiredSubscriptions) {
        {
            std::lock_guard lock(subscription_mutex_);
            subscription_registration_in_progress_ = true;
        }
        const auto result = transport_->Subscribe(subscription.topic_filter, subscription.qos);
        std::optional<std::vector<int>> early_ack;
        {
            std::lock_guard lock(subscription_mutex_);
            subscription_registration_in_progress_ = false;
            if (result) {
                pending_subscriptions_.emplace(result.packet_id,
                                               std::pair{ std::string(subscription.topic_filter), subscription.qos });
                const auto early = early_subscription_acknowledgements_.find(result.packet_id);
                if (early != early_subscription_acknowledgements_.end()) {
                    early_ack = std::move(early->second);
                }
            }
            early_subscription_acknowledgements_.clear();
        }
        if (!result) {
            subscription_failed_ = true;
            failed = true;
            Log(MqttLogLevel::kError,
                "MQTT subscription failed for " + std::string(subscription.topic_filter) + ": " + result.message);
        } else if (early_ack.has_value()) {
            HandleSubscribeAcknowledged(result.packet_id, *early_ack);
        }
    }
    bool all_acknowledged = false;
    {
        std::lock_guard lock(subscription_mutex_);
        subscriptions_registration_complete_ = true;
        all_acknowledged = pending_subscriptions_.empty();
    }
    if (failed) {
        ready_ = false;
        const auto reconnect = transport_->RequestReconnect();
        if (!reconnect) {
            Log(MqttLogLevel::kError, "MQTT reconnect request after subscribe failure failed: " + reconnect.message);
        }
    } else if (all_acknowledged && !subscription_failed_) {
        ready_ = true;
        Log(MqttLogLevel::kInfo, "MQTT subscriptions acknowledged; client is ready");
        static_cast<void>(PublishServerStatus(mqtt::ConnectionState::kOnline));
        static_cast<void>(PublishHeartbeat());
    }
}

contracts::mqtt::MqttMessage MqttClient::MakeServerStatusMessage(mqtt::ConnectionState status) {
    const std::string state = std::string(mqtt::ToString(status));
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "SERVER-STATUS-" + session_id_ + "-" + state + "-" + std::to_string(++server_message_sequence_),
        .message_type = mqtt::MessageType::kDeviceStatus,
        .source_id = config_.client_id,
        .timestamp = CurrentIso8601Timestamp(),
        .data =
            mqtt::DeviceStatusPayload{
                .status = status,
                .current_state = state,
                .job_id = std::nullopt,
                .error_code = std::nullopt,
                .departure_position = std::nullopt,
                .target_position = std::nullopt,
                .confirmed_position = std::nullopt,
                .movement_state = std::nullopt,
                .position_reset = false,
            },
    };
}

bool MqttClient::PublishServerStatus(mqtt::ConnectionState status) {
    return PublishMessage(mqtt::kServerStatusTopic, MakeServerStatusMessage(status), mqtt::Qos::kAtLeastOnce, true);
}

bool MqttClient::PublishHeartbeat() {
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "SERVER-HEARTBEAT-" + session_id_ + "-" + std::to_string(++server_message_sequence_),
        .message_type = mqtt::MessageType::kHeartbeat,
        .source_id = config_.client_id,
        .timestamp = CurrentIso8601Timestamp(),
        .data =
            mqtt::HeartbeatPayload{
                .status = mqtt::ConnectionState::kOnline,
                .current_state = "ONLINE",
                .uptime = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_at_)
                        .count()),
                .job_id = std::nullopt,
                .error_code = std::nullopt,
            },
    };
    return PublishMessage(mqtt::kServerHeartbeatTopic, message, mqtt::Qos::kAtMostOnce);
}

void MqttClient::StartHeartbeatLoop() {
    heartbeat_thread_ = std::jthread([this](std::stop_token stop_token) {
        const auto interval = mqtt::kHeartbeatInterval;
        while (!stop_token.stop_requested()) {
            const auto deadline = std::chrono::steady_clock::now() + interval;
            while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!stop_token.stop_requested() && connected_ && !stopping_) {
                static_cast<void>(PublishHeartbeat());
            }
        }
    });
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
