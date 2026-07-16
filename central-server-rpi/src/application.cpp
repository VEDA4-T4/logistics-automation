#include "logistics/central_server/application.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/mqtt_client.hpp"
#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/central_server/mqtt_transport.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"

namespace logistics::central_server {
namespace {

namespace mqtt = contracts::mqtt;

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) {
    stop_requested = 1;
}

[[nodiscard]] std::filesystem::path ResolveConfigPath(int argc, char* argv[]) {
    if (argc > 1 && argv[1] != nullptr && std::string_view(argv[1]).size() != 0) {
        return argv[1];
    }
    if (const char* environment_path = std::getenv("LOGISTICS_CENTRAL_SERVER_CONFIG");
        environment_path != nullptr && *environment_path != '\0') {
        return environment_path;
    }
    return std::filesystem::path("config") / "server.ini";
}

[[nodiscard]] std::string CurrentIso8601Timestamp() {
    const std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
    {
        static std::mutex time_mutex;
        std::lock_guard lock(time_mutex);
        const std::tm* converted = std::gmtime(&current_time);
        if (converted == nullptr) {
            return {};
        }
        utc_time = *converted;
    }

    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] constexpr bool IsDeviceRegistryMessage(mqtt::MessageType type) noexcept {
    return type == mqtt::MessageType::kDeviceRegister || type == mqtt::MessageType::kHeartbeat ||
           type == mqtt::MessageType::kDeviceStatus || type == mqtt::MessageType::kErrorOccurred;
}

}  // namespace

int Application::Run(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "usage: logistics_central_server [server.ini]\n";
        return 2;
    }

    const auto config_path = ResolveConfigPath(argc, argv);
    MqttConfig mqtt_config;
    try {
        mqtt_config = LoadMqttConfig(config_path);
    } catch (const ConfigError& error) {
        std::cerr << "[server][ERROR] " << error.what() << '\n';
        return 1;
    }

    std::unique_ptr<DeviceManager> device_manager;
    try {
        device_manager = std::make_unique<DeviceManager>(mqtt_config.device_registry_path);
    } catch (const DeviceRegistryError& error) {
        std::cerr << "[server][ERROR] " << error.what() << '\n';
        return 1;
    }
    MqttClient mqtt_client(std::move(mqtt_config), CreateMosquittoTransport());
    mqtt_client.SetMessageHandler([&device_manager](std::string_view topic, std::string_view payload) {
        const auto decoded = mqtt::DeserializeMessage(payload);
        if (!decoded.IsSuccess()) {
            std::cerr << "[server][ERROR] invalid MQTT JSON; error=" << mqtt::ToString(decoded.status.error)
                      << "; field=" << decoded.status.field << "; message=" << decoded.status.message << '\n';
            return;
        }

        const auto validation = mqtt::ValidateTopicMessage(topic, decoded.value);
        if (!validation.IsSuccess()) {
            std::cerr << "[server][ERROR] MQTT topic/message mismatch; error=" << mqtt::ToString(validation.error)
                      << "; message=" << validation.message << '\n';
            return;
        }

        const auto parsed_topic = mqtt::ParseTopic(topic);
        if (IsDeviceRegistryMessage(decoded.value.message_type) &&
            !device_manager->HandleMessage(parsed_topic, decoded.value, CurrentIso8601Timestamp())) {
            std::cerr << "[server][ERROR] device registry update failed: " << device_manager->LastError() << '\n';
        }
        std::clog << "[server][INFO] MQTT message received: " << topic << '\n';
        if (decoded.value.message_type == mqtt::MessageType::kDeviceRegister) {
            std::clog << "[server][INFO] device registered: " << parsed_topic.endpoint_id
                      << "; registered devices=" << device_manager->RegisteredDeviceCount() << '\n';
        }
    });

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (!mqtt_client.Start()) {
        return 1;
    }

    std::clog << "[server][INFO] central server started; registered devices=" << device_manager->RegisteredDeviceCount()
              << '\n';
    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    mqtt_client.Stop();
    std::clog << "[server][INFO] central server stopped\n";
    return 0;
}

}  // namespace logistics::central_server
