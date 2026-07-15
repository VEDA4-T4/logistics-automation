#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace logistics::central_server {

struct MqttConfig final {
    std::string host;
    std::string client_id;
    std::string username;
    std::string password;
    std::uint16_t port{ 1883 };
    std::uint16_t keep_alive_seconds{ 30 };
    std::uint32_t reconnect_min_delay_seconds{ 1 };
    std::uint32_t reconnect_max_delay_seconds{ 30 };
    bool clean_session{ true };

    [[nodiscard]] bool IsValid() const noexcept;
};

class ConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] MqttConfig LoadMqttConfig(const std::filesystem::path& path);

}  // namespace logistics::central_server
