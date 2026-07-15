#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/central_server/mqtt_transport.hpp"

namespace logistics::contracts::mqtt {

struct MqttMessage;
enum class Qos : std::uint8_t;

}  // namespace logistics::contracts::mqtt

namespace logistics::central_server {

enum class MqttLogLevel : std::uint8_t {
    kInfo,
    kWarning,
    kError,
};

class MqttClient final {
public:
    using Logger = std::function<void(MqttLogLevel level, std::string_view message)>;
    using MessageHandler = std::function<void(std::string_view topic, std::string_view payload)>;

    MqttClient(MqttConfig config, std::unique_ptr<MqttTransport> transport);
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    MqttClient(MqttClient&&) = delete;
    MqttClient& operator=(MqttClient&&) = delete;

    void SetLogger(Logger logger);
    void SetMessageHandler(MessageHandler handler);
    [[nodiscard]] bool SetWill(MqttWill will);

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;

    [[nodiscard]] bool Publish(std::string_view topic, std::string_view payload, int qos = 1, bool retain = false);
    [[nodiscard]] bool PublishMessage(std::string_view topic, const contracts::mqtt::MqttMessage& message,
                                      contracts::mqtt::Qos qos, bool retain = false);

private:
    void HandleConnected(int reason_code, std::string_view reason);
    void HandleDisconnected(int reason_code, std::string_view reason);
    void HandleMessage(std::string_view topic, std::string_view payload);
    void HandleTransportLog(MqttTransportLogLevel level, std::string_view message);
    void SubscribeRequiredTopics();
    void Log(MqttLogLevel level, std::string_view message) const;

    MqttConfig config_;
    std::unique_ptr<MqttTransport> transport_;
    std::atomic_bool connected_{ false };
    std::atomic_bool running_{ false };
    std::atomic_bool stopping_{ false };
    mutable std::mutex callback_mutex_;
    Logger logger_;
    MessageHandler message_handler_;
    std::optional<MqttWill> will_;
};

}  // namespace logistics::central_server
