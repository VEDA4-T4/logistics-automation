#include "logistics/device/mqtt_node_client.hpp"

#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"
#include "logistics/device/mqtt_message_processor.hpp"
#include "logistics/device/mqtt_publish_spool.hpp"
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

[[nodiscard]] bool HasLegacySpoolRecords(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) {
            return true;
        }
        if (entry.is_regular_file(error) && entry.path().extension() == ".pending") {
            return true;
        }
    }
    return false;
}

}  // namespace

class MqttNodeClient::Impl final {
public:
    Impl(MqttNodeConfig config, std::string device_type, std::shared_ptr<DeviceStatus> device_status)
        : config_(std::move(config)),
          device_type_(std::move(device_type)),
          processor_(config_.device_id),
          message_session_id_(GenerateMessageSessionId()),
          device_status_(std::move(device_status)),
          publish_spool_(config_.publish_spool_directory / config_.device_id, config_.publish_spool_maximum_bytes,
                         config_.publish_spool_maximum_records),
          inbound_spool_(config_.publish_spool_directory / config_.device_id / "inbound",
                         config_.publish_spool_maximum_bytes, config_.publish_spool_maximum_records,
                         MqttSpoolOverflowPolicy::kRejectNew) {
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

    void SetWorkCreatedEpochReassignmentGuard(std::function<bool()> guard) {
        processor_.SetWorkCreatedEpochReassignmentGuard(std::move(guard));
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
        if (config_.clean_session) {
            std::cerr << "[device][mqtt][WARN] clean_session=true disables broker-side offline QoS1 replay\n";
        }
        if (!publish_spool_.Start()) {
            running_ = false;
            std::cerr << "[device][mqtt][ERROR] unable to initialize MQTT publish spool\n";
            return false;
        }
        if (HasLegacySpoolRecords(config_.publish_spool_directory)) {
            running_ = false;
            std::cerr << "[device][mqtt][ERROR] legacy root MQTT spool records detected; migrate them before start\n";
            return false;
        }
        if (!inbound_spool_.Start()) {
            running_ = false;
            std::cerr << "[device][mqtt][ERROR] unable to initialize inbound MQTT spool\n";
            return false;
        }
        spool_blocked_ = false;
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
        mosquitto_publish_callback_set(client_, &Impl::OnPublished);
        mosquitto_subscribe_callback_set(client_, &Impl::OnSubscribed);
        mosquitto_log_callback_set(client_, &Impl::OnLog);

        int result = mosquitto_int_option(client_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
        if (result == MOSQ_ERR_SUCCESS && !config_.username.empty()) {
            result = mosquitto_username_pw_set(client_, config_.username.c_str(),
                                               config_.password.empty() ? nullptr : config_.password.c_str());
        }
        if (result == MOSQ_ERR_SUCCESS && config_.tls_enabled) {
            if (config_.ca_certificate.empty()) {
                result = MOSQ_ERR_INVAL;
            } else {
                const auto ca_path = config_.ca_certificate.string();
                result = mosquitto_tls_set(client_, ca_path.c_str(), nullptr, nullptr, nullptr, nullptr);
            }
        }
        if (result == MOSQ_ERR_SUCCESS && config_.tls_enabled) {
            result = mosquitto_tls_opts_set(client_, 1, "tlsv1.2", nullptr);
        }
        if (result == MOSQ_ERR_SUCCESS && config_.tls_enabled) {
            result = mosquitto_tls_insecure_set(client_, false);
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
        ready_ = false;
        command_subscription_ready_ = false;
        broadcast_subscription_ready_ = false;
        lifecycle_gate_ = false;
        {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            lifecycle_active_ = false;
            lifecycle_pending_ = false;
            lifecycle_online_queued_ = false;
            lifecycle_registration_queued_ = false;
            lifecycle_inflight_ids_.clear();
        }
        const std::string timestamp = CurrentIso8601Timestamp();
        try {
            device_status_->SetConnectionState(mqtt::ConnectionState::kOffline);
        } catch (...) {
        }
        if (client_ == nullptr) {
            return;
        }

        bool offline_queued = false;
        bool offline_acknowledged = false;
        std::string offline_record_id;
        if (was_connected) {
            try {
                const auto offline =
                    processor_.EncodeOfflineStatus(LifecycleMessageId("OFFLINE", connection_generation_), timestamp);
                if (offline.IsSuccess() && offline.payload.size() <= static_cast<std::size_t>(INT_MAX)) {
                    const auto record =
                        publish_spool_.Enqueue(mqtt::DeviceStatusTopic(config_.device_id), offline.payload, 1, true);
                    offline_queued = record.has_value();
                    if (record.has_value()) {
                        offline_record_id = record->id;
                    }
                }
                if (offline_queued) {
                    PumpPublishSpool(true);
                    device_status_->MarkCommunication(timestamp);
                    DrainPublishSpoolForShutdown();
                    offline_acknowledged = !publish_spool_.IsPending(offline_record_id);
                    if (!offline_acknowledged) {
                        const bool quarantined = publish_spool_.Quarantine(offline_record_id);
                        if (!quarantined && publish_spool_.IsPending(offline_record_id)) {
                            std::cerr << "[device][mqtt][ERROR] unable to obsolete undelivered offline status "
                                      << offline_record_id << "; it will replay before next online status\n";
                        }
                        if (!quarantined && !publish_spool_.IsPending(offline_record_id)) {
                            // The PUBACK callback may have won the race with quarantine.
                            offline_acknowledged = true;
                        }
                    }
                }
            } catch (...) {
                std::cerr << "[device][mqtt][ERROR] unable to prepare offline status during shutdown\n";
            }
        }

        const int disconnect_result = offline_acknowledged ? mosquitto_disconnect(client_) : MOSQ_ERR_NO_CONN;
        const bool graceful_disconnect_started = disconnect_result == MOSQ_ERR_SUCCESS;
        if (loop_started_) {
            static_cast<void>(mosquitto_loop_stop(client_, !graceful_disconnect_started));
            loop_started_ = false;
        }
        mosquitto_destroy(client_);
        client_ = nullptr;
    }

    [[nodiscard]] bool IsConnected() const noexcept {
        return ready_;
    }

    [[nodiscard]] bool PublishHeartbeat(std::string message_id, std::string timestamp) {
        if (!ready_ || client_ == nullptr) {
            return false;
        }

        const auto status = device_status_->Snapshot();
        const auto encoded = processor_.EncodeHeartbeat(std::move(message_id), timestamp, status.current_state,
                                                        status.uptime, status.job_id, status.error_code);
        const std::string topic = mqtt::DeviceHeartbeatTopic(config_.device_id);
        const bool published = PublishVolatile(topic, encoded, 0, false, "heartbeat");
        if (published) {
            device_status_->MarkCommunication(std::move(timestamp));
        }
        return published;
    }

    [[nodiscard]] bool PublishResponse(const mqtt::MqttMessage& message) {
        const auto prepared = processor_.PrepareOutboundMessage(message);
        if (!prepared.has_value()) {
            return false;
        }
        processor_.RememberCommandResponse(*prepared);
        return PublishMessage(mqtt::DeviceResponseTopic(config_.device_id), *prepared, 1, false, "command response");
    }

    [[nodiscard]] bool PublishStatus(const mqtt::MqttMessage& message) {
        const auto prepared = processor_.PrepareOutboundMessage(message);
        return prepared.has_value() &&
               PublishMessage(mqtt::DeviceStatusTopic(config_.device_id), *prepared, 1, true, "device status");
    }

    [[nodiscard]] bool PublishEvent(const mqtt::MqttMessage& message) {
        const auto prepared = processor_.PrepareOutboundMessage(message);
        if (!prepared.has_value()) {
            return false;
        }
        const std::string topic = mqtt::DeviceEventTopic(config_.device_id);
        const auto validation = mqtt::ValidateTopicMessage(topic, *prepared);
        if (!validation.IsSuccess()) {
            std::cerr << "[device][mqtt][ERROR] invalid device event: " << validation.message << '\n';
            return false;
        }
        const auto encoded = mqtt::SerializeMessage(*prepared);
        if (!encoded.IsSuccess()) {
            return false;
        }
        const bool delivered =
            DeliverEvent(publish_spool_, prepared->message_type, topic, encoded.payload,
                         [this](std::string_view volatile_topic, std::string_view payload, int qos, bool retain) {
                             return ready_ && client_ != nullptr &&
                                    PublishVolatile(std::string(volatile_topic),
                                                    mqtt::EncodeResult{ .payload = std::string(payload), .status = {} },
                                                    qos, retain, "sensor telemetry");
                         });
        if (!delivered) {
            if (!mqtt::IsTransientTelemetry(prepared->message_type)) {
                std::cerr << "[device][mqtt][ERROR] unable to persist device event for MQTT publication\n";
            }
            return false;
        }
        device_status_->MarkCommunication(prepared->timestamp);
        if (!mqtt::IsTransientTelemetry(prepared->message_type)) {
            PumpPublishSpool();
        }
        return true;
    }

    [[nodiscard]] bool PublishError(const mqtt::MqttMessage& message) {
        const auto prepared = processor_.PrepareOutboundMessage(message);
        return prepared.has_value() &&
               PublishMessage(mqtt::DeviceErrorTopic(config_.device_id), *prepared, 1, false, "device error");
    }

private:
    [[nodiscard]] std::string LifecycleMessageId(std::string_view kind, std::uint64_t sequence) const {
        return std::string(kind) + '-' + MakeMessageId(config_.device_id, message_session_id_, sequence);
    }

    [[nodiscard]] bool PublishVolatile(const std::string& topic, const mqtt::EncodeResult& encoded, const int qos,
                                       const bool retain, std::string_view description) {
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

    [[nodiscard]] bool PublishMessage(const std::string& topic, const mqtt::MqttMessage& message, int qos, bool retain,
                                      std::string_view description) {
        const auto validation = mqtt::ValidateTopicMessage(topic, message);
        if (!validation.IsSuccess()) {
            std::cerr << "[device][mqtt][ERROR] invalid " << description << ": " << validation.message << '\n';
            return false;
        }

        const auto encoded = mqtt::SerializeMessage(message);
        if (!encoded.IsSuccess()) {
            return false;
        }
        const auto record = publish_spool_.Enqueue(topic, encoded.payload, qos, retain);
        if (!record.has_value()) {
            std::cerr << "[device][mqtt][ERROR] unable to persist " << description << " for MQTT publication\n";
            return false;
        }
        device_status_->MarkCommunication(message.timestamp);
        PumpPublishSpool();
        return true;
    }

    [[nodiscard]] bool PublishOnlineStatusAndRegistration(std::string_view timestamp) {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!lifecycle_active_) {
            lifecycle_generation_ = ++connection_generation_;
            lifecycle_active_ = true;
            lifecycle_online_queued_ = false;
            lifecycle_registration_queued_ = false;
            lifecycle_inflight_ids_.clear();
        }
        const auto generation = lifecycle_generation_;
        const auto status = device_status_->Snapshot();

        bool online_published = lifecycle_online_queued_;
        std::string online_record_id;
        if (!online_published) {
            const auto online = processor_.EncodeOnlineStatus(LifecycleMessageId("STATUS", generation),
                                                              std::string(timestamp), status.current_state);
            online_published = QueueEncoded(mqtt::DeviceStatusTopic(config_.device_id), online, true, "online status",
                                            false, &online_record_id, false);
            lifecycle_online_queued_ = online_published;
            if (online_published) {
                lifecycle_inflight_ids_.insert(online_record_id);
            }
        }

        bool registration_published = lifecycle_registration_queued_;
        std::string registration_record_id;
        if (!registration_published) {
            const auto registration = processor_.EncodeDeviceRegistration(
                LifecycleMessageId("REGISTER", generation), std::string(timestamp), device_type_, config_.node_name,
                config_.ip_address, status.uart_connected);
            registration_published = QueueEncoded(mqtt::DeviceRegisterTopic(config_.device_id), registration, false,
                                                  "device registration", false, &registration_record_id, false);
            lifecycle_registration_queued_ = registration_published;
            if (registration_published) {
                lifecycle_inflight_ids_.insert(registration_record_id);
            }
        }
        if (online_published && registration_published) {
            PumpPublishSpool();
        }
        return online_published && registration_published;
    }

    [[nodiscard]] bool QueueEncoded(const std::string& topic, const mqtt::EncodeResult& encoded, const bool retain,
                                    std::string_view description, const bool allow_stopping = false,
                                    std::string* record_id = nullptr, const bool pump = true) {
        if (!encoded.IsSuccess()) {
            return false;
        }
        const auto record = publish_spool_.Enqueue(topic, encoded.payload, 1, retain);
        if (!record.has_value()) {
            std::cerr << "[device][mqtt][ERROR] unable to persist " << description << " for MQTT publication\n";
            return false;
        }
        if (record_id != nullptr) {
            *record_id = record->id;
        }
        if (pump) {
            PumpPublishSpool(allow_stopping);
        }
        return true;
    }

    void PumpPublishSpool(const bool allow_stopping = false) {
        std::lock_guard lock(publish_mutex_);
        if (spool_blocked_ || (!ready_ && !allow_stopping) || client_ == nullptr || !inflight_publishes_.empty()) {
            return;
        }
        const auto record = publish_spool_.Next();
        if (!record.has_value()) {
            return;
        }
        int mid{};
        if (record->payload.empty() || record->payload.size() > static_cast<std::size_t>(INT_MAX) || record->qos != 1) {
            std::cerr << "[device][mqtt][ERROR] invalid durable MQTT record " << record->id << "; quarantining\n";
            static_cast<void>(publish_spool_.Quarantine(record->id));
            return;
        }
        const int result =
            mosquitto_publish(client_, &mid, record->topic.c_str(), static_cast<int>(record->payload.size()),
                              record->payload.data(), record->qos, record->retain);
        if (result == MOSQ_ERR_SUCCESS) {
            inflight_publishes_.emplace(mid, record->id);
        } else {
            std::cerr << "[device][mqtt][WARN] durable publish deferred: " << ErrorText(result) << '\n';
        }
    }

    void DrainPublishSpoolForShutdown() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            PumpPublishSpool(true);
            bool inflight = false;
            {
                std::lock_guard lock(publish_mutex_);
                inflight = !inflight_publishes_.empty();
            }
            if (!inflight && publish_spool_.PendingCount() == 0U) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void QueueLifecycleOrRetry(std::string_view timestamp) {
        const bool complete = PublishOnlineStatusAndRegistration(timestamp);
        {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            lifecycle_pending_ = !complete;
        }
        if (complete) {
            return;
        }
        std::cerr << "[device][mqtt][WARN] MQTT lifecycle publication remains queued for retry\n";
        PumpPublishSpool();
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
        ready_ = false;
        spool_blocked_ = false;
        command_subscription_ready_ = false;
        broadcast_subscription_ready_ = false;
        const std::string timestamp = CurrentIso8601Timestamp();
        device_status_->SetConnectionState(mqtt::ConnectionState::kReconnecting);
        device_status_->MarkCommunication(timestamp);
        const std::string command_topic = mqtt::DeviceCommandTopic(config_.device_id);
        const std::string broadcast_topic(mqtt::kSystemBroadcastCommandTopic);
        int command_mid{};
        int broadcast_mid{};
        int command_result{};
        int broadcast_result{};
        {
            std::lock_guard lock(publish_mutex_);
            command_result = mosquitto_subscribe(client_, &command_mid, command_topic.c_str(), 1);
            broadcast_result = mosquitto_subscribe(client_, &broadcast_mid, broadcast_topic.c_str(), 1);
            if (command_result == MOSQ_ERR_SUCCESS) {
                pending_subscriptions_.emplace(command_mid, false);
            }
            if (broadcast_result == MOSQ_ERR_SUCCESS) {
                pending_subscriptions_.emplace(broadcast_mid, true);
            }
        }
        if (command_result != MOSQ_ERR_SUCCESS || broadcast_result != MOSQ_ERR_SUCCESS) {
            {
                std::lock_guard lock(publish_mutex_);
                pending_subscriptions_.clear();
            }
            ready_ = false;
            std::cerr << "[device][mqtt][ERROR] MQTT command subscription failed\n";
            retry_after_disconnect_ = true;
            const int disconnect = mosquitto_disconnect(client_);
            if (disconnect != MOSQ_ERR_SUCCESS) {
                retry_after_disconnect_ = false;
                static_cast<void>(mosquitto_reconnect_async(client_));
            }
        }
    }

    void HandleDisconnected(int reason_code) {
        connected_ = false;
        ready_ = false;
        command_subscription_ready_ = false;
        broadcast_subscription_ready_ = false;
        lifecycle_gate_ = false;
        {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            lifecycle_pending_ = false;
            lifecycle_active_ = false;
            lifecycle_online_queued_ = false;
            lifecycle_registration_queued_ = false;
        }
        {
            std::lock_guard lock(publish_mutex_);
            inflight_publishes_.clear();
            pending_subscriptions_.clear();
        }
        const std::string timestamp = CurrentIso8601Timestamp();
        device_status_->SetConnectionState(running_ ? mqtt::ConnectionState::kReconnecting
                                                    : mqtt::ConnectionState::kOffline);
        device_status_->MarkCommunication(timestamp);
        if (running_) {
            std::cerr << "[device][mqtt][WARN] MQTT broker disconnected (" << reason_code
                      << "); automatic reconnect is active\n";
            if (retry_after_disconnect_.exchange(false)) {
                const int reconnect = mosquitto_reconnect_async(client_);
                if (reconnect != MOSQ_ERR_SUCCESS) {
                    std::cerr << "[device][mqtt][ERROR] unable to retry MQTT subscription: " << ErrorText(reconnect)
                              << '\n';
                }
            }
        }
    }

    void HandleMessage(const mosquitto_message& received) {
        if (received.topic == nullptr || received.payloadlen < 0) {
            return;
        }
        const char* bytes = received.payload == nullptr ? "" : static_cast<const char*>(received.payload);
        const std::string topic(received.topic);
        const std::string payload(bytes, static_cast<std::size_t>(received.payloadlen));
        // Persist every delivery first. A persistent session may replay QoS1 commands
        // before SUBACK/lifecycle PUBACK, and handler failure must remain retryable.
        if (!inbound_spool_.Enqueue(topic, payload, 1, false).has_value()) {
            std::cerr << "[device][mqtt][ERROR] unable to persist inbound MQTT command; stopping for safety\n";
            retry_after_disconnect_ = true;
            static_cast<void>(mosquitto_disconnect(client_));
            return;
        }
        if (ready_ && !lifecycle_gate_) {
            DrainPendingMessages();
        }
    }

    [[nodiscard]] bool ProcessReceivedMessage(const std::string& topic, const std::string& payload) {
        device_status_->MarkCommunication(CurrentIso8601Timestamp());
        const auto decoded = processor_.DecodeCommand(topic, payload);
        if (!decoded.IsSuccess()) {
            std::cerr << "[device][mqtt][ERROR] " << decoded.error << '\n';
            return true;
        }
        if (decoded.duplicate) {
            const auto cached_response = processor_.CachedCommandResponse(decoded.message);
            if (cached_response.has_value()) {
                std::clog << "[device][mqtt][INFO] cached response replayed for duplicate MQTT command: "
                          << decoded.message.message_id << '\n';
                const bool published = PublishMessage(mqtt::DeviceResponseTopic(config_.device_id), *cached_response, 1,
                                                      false, "cached command response");
                if (!published) {
                    processor_.ForgetCommand(decoded.message.message_id);
                }
                return published;
            } else {
                const auto* work = mqtt::GetPayload<mqtt::WorkCreatedPayload>(decoded.message);
                const auto status = device_status_->Snapshot();
                if (decoded.message.message_type == mqtt::MessageType::kWorkCreated && work != nullptr &&
                    status.job_id.has_value() && *status.job_id == work->work_id) {
                    const mqtt::MqttMessage assignment_status{
                        .message_id = MakeMessageId(config_.device_id, message_session_id_,
                                                    duplicate_status_sequence_.fetch_add(1)),
                        .message_type = mqtt::MessageType::kDeviceStatus,
                        .source_id = config_.device_id,
                        .timestamp = CurrentIso8601Timestamp(),
                        .data =
                            mqtt::DeviceStatusPayload{
                                .status = mqtt::ConnectionState::kOnline,
                                .current_state = "WORK_ASSIGNED",
                                .job_id = status.job_id,
                                .error_code = std::nullopt,
                                .departure_position = std::nullopt,
                                .target_position = std::nullopt,
                                .confirmed_position = std::nullopt,
                                .movement_state = std::nullopt,
                                .position_reset = false,
                            },
                    };
                    return PublishStatus(assignment_status);
                }
                std::clog << "[device][mqtt][INFO] duplicate MQTT command is still pending: "
                          << decoded.message.message_id << '\n';
            }
            processor_.ForgetCommand(decoded.message.message_id);
            return false;
        }

        CommandHandler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = command_handler_;
        }
        if (handler) {
            try {
                const bool handled = handler(decoded.message);
                if (!handled) {
                    processor_.ForgetCommand(decoded.message.message_id);
                }
                return handled;
            } catch (const std::exception& error) {
                std::cerr << "[device][mqtt][ERROR] command handler failed; retaining inbound command: " << error.what()
                          << '\n';
            } catch (...) {
                std::cerr << "[device][mqtt][ERROR] command handler failed; retaining inbound command\n";
            }
        }
        processor_.ForgetCommand(decoded.message.message_id);
        return false;
    }

    void DrainPendingMessages() {
        for (;;) {
            const auto message = inbound_spool_.Next();
            if (!message.has_value()) {
                break;
            }
            if (!ProcessReceivedMessage(message->topic, message->payload)) {
                return;
            }
            if (!inbound_spool_.Acknowledge(message->id)) {
                std::cerr << "[device][mqtt][ERROR] unable to acknowledge inbound MQTT command " << message->id
                          << "; drain is halted\n";
                return;
            }
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

    static void OnPublished(mosquitto*, void* object, const int mid) {
        auto* self = static_cast<Impl*>(object);
        std::string record_id;
        {
            std::lock_guard lock(self->publish_mutex_);
            const auto it = self->inflight_publishes_.find(mid);
            if (it == self->inflight_publishes_.end()) {
                return;
            }
            record_id = it->second;
            // Keep the publish slot occupied until its durable record has been
            // removed. Otherwise another thread can select and publish the
            // same .pending file between map erasure and acknowledgement.
            if (!self->publish_spool_.Acknowledge(record_id)) {
                self->inflight_publishes_.erase(it);
                self->spool_blocked_ = true;
                std::cerr << "[device][mqtt][ERROR] unable to remove PUBACKed MQTT record " << record_id
                          << "; durable spool pump is halted\n";
                return;
            }
            self->inflight_publishes_.erase(it);
            self->spool_blocked_ = false;
        }
        bool lifecycle_acknowledged = false;
        {
            std::lock_guard lifecycle_lock(self->lifecycle_mutex_);
            lifecycle_acknowledged = self->lifecycle_inflight_ids_.erase(record_id) > 0U;
        }
        if (self->lifecycle_pending_ && self->ready_) {
            self->QueueLifecycleOrRetry(CurrentIso8601Timestamp());
        }
        bool lifecycle_complete = false;
        {
            std::lock_guard lifecycle_lock(self->lifecycle_mutex_);
            lifecycle_complete =
                lifecycle_acknowledged && !self->lifecycle_pending_ && self->lifecycle_inflight_ids_.empty();
        }
        if (lifecycle_complete) {
            self->lifecycle_gate_ = false;
            self->DrainPendingMessages();
        }
        self->PumpPublishSpool();
    }

    static void OnSubscribed(mosquitto*, void* object, const int mid, const int qos_count, const int* granted_qos) {
        auto* self = static_cast<Impl*>(object);
        const bool accepted = qos_count == 1 && granted_qos != nullptr && granted_qos[0] >= 1 && granted_qos[0] != 0x80;
        bool all_subscribed = false;
        {
            std::lock_guard lock(self->publish_mutex_);
            const auto subscription = self->pending_subscriptions_.find(mid);
            if (subscription == self->pending_subscriptions_.end()) {
                return;
            }
            if (accepted) {
                if (subscription->second) {
                    self->broadcast_subscription_ready_ = true;
                } else {
                    self->command_subscription_ready_ = true;
                }
            }
            self->pending_subscriptions_.erase(subscription);
            all_subscribed = self->pending_subscriptions_.empty();
        }
        if (!accepted) {
            self->connected_ = false;
            self->ready_ = false;
            self->device_status_->SetConnectionState(mqtt::ConnectionState::kReconnecting);
            std::cerr << "[device][mqtt][ERROR] MQTT command subscription rejected\n";
            self->retry_after_disconnect_ = true;
            const int disconnect = mosquitto_disconnect(self->client_);
            if (disconnect != MOSQ_ERR_SUCCESS) {
                self->retry_after_disconnect_ = false;
                static_cast<void>(mosquitto_reconnect_async(self->client_));
            }
            return;
        }
        if (!all_subscribed) {
            return;
        }
        self->ready_ = true;
        self->lifecycle_gate_ = true;
        self->device_status_->SetConnectionState(mqtt::ConnectionState::kOnline);
        const std::string timestamp = CurrentIso8601Timestamp();
        self->QueueLifecycleOrRetry(timestamp);
        bool lifecycle_complete = false;
        {
            std::lock_guard lifecycle_lock(self->lifecycle_mutex_);
            lifecycle_complete = !self->lifecycle_pending_ && self->lifecycle_inflight_ids_.empty();
        }
        if (lifecycle_complete) {
            self->lifecycle_gate_ = false;
            self->DrainPendingMessages();
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
    std::atomic_uint64_t duplicate_status_sequence_{ 1 };
    std::shared_ptr<DeviceStatus> device_status_;
    mosquitto* client_{ nullptr };
    std::atomic_bool running_{ false };
    std::atomic_bool connected_{ false };
    std::atomic_bool ready_{ false };
    bool loop_started_{ false };
    MqttPublishSpool publish_spool_;
    MqttPublishSpool inbound_spool_;
    std::map<int, std::string> inflight_publishes_;
    std::map<int, bool> pending_subscriptions_;
    std::mutex publish_mutex_;
    std::atomic_bool lifecycle_pending_{ false };
    std::atomic_bool retry_after_disconnect_{ false };
    std::atomic_bool command_subscription_ready_{ false };
    std::atomic_bool broadcast_subscription_ready_{ false };
    std::atomic_bool lifecycle_gate_{ false };
    std::atomic_bool spool_blocked_{ false };
    bool lifecycle_active_{ false };
    bool lifecycle_online_queued_{ false };
    bool lifecycle_registration_queued_{ false };
    std::set<std::string> lifecycle_inflight_ids_;
    std::uint64_t lifecycle_generation_{};
    std::mutex lifecycle_mutex_;
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

void MqttNodeClient::SetWorkCreatedEpochReassignmentGuard(std::function<bool()> guard) {
    impl_->SetWorkCreatedEpochReassignmentGuard(std::move(guard));
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

bool MqttNodeClient::PublishResponse(const contracts::mqtt::MqttMessage& message) {
    return impl_->PublishResponse(message);
}

bool MqttNodeClient::PublishStatus(const contracts::mqtt::MqttMessage& message) {
    return impl_->PublishStatus(message);
}

bool MqttNodeClient::PublishEvent(const contracts::mqtt::MqttMessage& message) {
    return impl_->PublishEvent(message);
}

bool MqttNodeClient::PublishError(const contracts::mqtt::MqttMessage& message) {
    return impl_->PublishError(message);
}

}  // namespace logistics::device
