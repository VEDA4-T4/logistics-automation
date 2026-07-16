#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace logistics::central_server {

class DeviceManager;

enum class MqttHandlerLogLevel : std::uint8_t {
    kInfo,
    kError,
};

class MqttHandler final {
public:
    using Logger = std::function<void(MqttHandlerLogLevel level, std::string_view message)>;

    explicit MqttHandler(DeviceManager& device_manager, Logger logger = {});

    [[nodiscard]] bool Handle(std::string_view topic, std::string_view payload,
                              std::string_view received_at = {});

private:
    void Log(MqttHandlerLogLevel level, std::string_view message) const;

    DeviceManager& device_manager_;
    Logger logger_;
};

}  // namespace logistics::central_server
