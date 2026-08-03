#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace logistics::central_server {

struct MqttWill final {
    std::string topic;
    std::string payload;
    int qos{ 1 };
    bool retain{ true };
};

struct MqttTransportOptions final {
    std::string host;
    std::string client_id;
    std::string username;
    std::string password;
    std::uint16_t port{ 1883 };
    std::uint16_t keep_alive_seconds{ 30 };
    std::uint32_t reconnect_min_delay_seconds{ 1 };
    std::uint32_t reconnect_max_delay_seconds{ 30 };
    bool clean_session{ true };
    bool tls_enabled{false};
    std::string ca_certificate;
    std::optional<MqttWill> will;
};

struct MqttOperationResult final {
    int code{};
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code == 0;
    }
};

enum class MqttTransportLogLevel : std::uint8_t {
    kDebug,
    kInfo,
    kWarning,
    kError,
};

struct MqttTransportCallbacks final {
    std::function<void(int reason_code, std::string_view reason)> connected;
    std::function<void(int reason_code, std::string_view reason)> disconnected;
    std::function<void(std::string_view topic, std::string_view payload)> message_received;
    std::function<void(MqttTransportLogLevel level, std::string_view message)> log_received;
};

class MqttTransport {
public:
    virtual ~MqttTransport() = default;

    virtual void SetCallbacks(MqttTransportCallbacks callbacks) = 0;
    [[nodiscard]] virtual MqttOperationResult Start(const MqttTransportOptions& options) = 0;
    virtual void Stop() noexcept = 0;
    [[nodiscard]] virtual MqttOperationResult Publish(std::string_view topic, std::string_view payload, int qos,
                                                      bool retain) = 0;
    [[nodiscard]] virtual MqttOperationResult Subscribe(std::string_view topic_filter, int qos) = 0;
};

[[nodiscard]] std::unique_ptr<MqttTransport> CreateMosquittoTransport();

}  // namespace logistics::central_server
