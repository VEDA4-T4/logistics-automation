#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace logistics::device {

struct MqttNodeConfig final {
    std::string device_id;
    std::string node_name;
    std::string ip_address;
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

class NodeConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] MqttNodeConfig LoadMqttNodeConfig(const std::filesystem::path& path);

}  // namespace logistics::device
