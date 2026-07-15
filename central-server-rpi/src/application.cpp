#include "logistics/central_server/application.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

    DeviceManager device_manager;
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

        device_manager.HandleMessage(mqtt::ParseTopic(topic), decoded.value);
        std::clog << "[server][INFO] MQTT message received: " << topic << '\n';
    });

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (!mqtt_client.Start()) {
        return 1;
    }

    std::clog << "[server][INFO] central server started; registered devices=" << device_manager.RegisteredDeviceCount()
              << '\n';
    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    mqtt_client.Stop();
    std::clog << "[server][INFO] central server stopped\n";
    return 0;
}

}  // namespace logistics::central_server
