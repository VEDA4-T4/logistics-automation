#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "logistics/device/image_uploader.hpp"
#include "logistics/device/log_spool_uploader.hpp"

namespace logistics::device {

struct MqttNodeConfig final {
    std::string device_id;
    std::string node_name;
    std::string ip_address;
    std::string host;
    std::string client_id;
    std::string username;
    std::string password;
    std::filesystem::path ca_certificate;
    std::uint16_t port{ 1883 };
    std::uint16_t keep_alive_seconds{ 30 };
    std::uint32_t reconnect_min_delay_seconds{ 1 };
    std::uint32_t reconnect_max_delay_seconds{ 30 };
    // Root directory; the client appends device_id to isolate node processes.
    std::filesystem::path publish_spool_directory{ "/var/lib/logistics/mqtt-spool" };
    std::size_t publish_spool_maximum_bytes{ 50U * 1024U * 1024U };
    std::uint8_t sorting_default_speed{ 50 };
    // Persistent sessions preserve broker-side QoS1 deliveries while a node reconnects.
    // Set true only when at-most-once delivery during offline periods is intentional.
    bool clean_session{ false };
    bool tls_enabled{ false };
    bool log_upload_enabled{ false };
    LogSpoolConfig log_upload;
    bool image_upload_enabled{ false };
    ImageUploadConfig image_upload;

    [[nodiscard]] bool IsValid() const noexcept;
};

class NodeConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] MqttNodeConfig LoadMqttNodeConfig(const std::filesystem::path& path);

}  // namespace logistics::device
