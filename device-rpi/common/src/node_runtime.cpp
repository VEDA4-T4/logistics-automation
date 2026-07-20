#include "logistics/device/node_runtime.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef LOGISTICS_DEVICE_MQTT_ENABLED
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/device_status.hpp"
#include "logistics/device/log_spool_uploader.hpp"
#include "logistics/device/mqtt_node_client.hpp"
#include "logistics/device/mqtt_node_config.hpp"
#include "logistics/device/mqtt_time.hpp"
#endif

namespace logistics::device {
namespace {

#ifdef LOGISTICS_DEVICE_MQTT_ENABLED
volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) {
    stop_requested = 1;
}

[[nodiscard]] std::filesystem::path ResolveConfigPath(int argc, char* argv[]) {
    if (argc > 1 && argv[1] != nullptr && std::string_view(argv[1]).size() != 0) {
        return argv[1];
    }
    if (const char* environment_path = std::getenv("LOGISTICS_DEVICE_CONFIG");
        environment_path != nullptr && *environment_path != '\0') {
        return environment_path;
    }
    return std::filesystem::path("device-rpi") / "config" / "node.ini";
}
#endif

}  // namespace

NodeRuntime::NodeRuntime(contracts::DeviceRole role) : role_(role) {}

int NodeRuntime::Run(int argc, char* argv[]) const {
#ifdef LOGISTICS_DEVICE_MQTT_ENABLED
    if (argc > 2) {
        std::cerr << "usage: logistics_device_node [node.ini]\n";
        return 2;
    }

    MqttNodeConfig config;
    try {
        config = LoadMqttNodeConfig(ResolveConfigPath(argc, argv));
    } catch (const NodeConfigError& error) {
        std::cerr << "[device][ERROR] " << error.what() << '\n';
        return 1;
    }

    const std::string device_id = config.device_id;
    std::unique_ptr<LogSpoolUploader> log_uploader;
    if (config.log_upload_enabled) {
        log_uploader = std::make_unique<LogSpoolUploader>(config.log_upload);
        if (!log_uploader->Start()) {
            std::cerr << "[device][ERROR] unable to start HTTP log spool uploader\n";
            return 1;
        }
        static_cast<void>(log_uploader->Append("device node starting"));
    }
    auto device_status = std::make_shared<DeviceStatus>(device_id);
    MqttNodeClient mqtt_client(std::move(config), std::string(contracts::ToString(role_)), device_status);
    mqtt_client.SetCommandHandler([uploader = log_uploader.get(), device_status](
                                      const contracts::mqtt::MqttMessage& message) {
        std::clog << "[device][INFO] MQTT command received: " << contracts::mqtt::ToString(message.message_type)
                  << '\n';
        if (uploader != nullptr) {
            static_cast<void>(uploader->Append("MQTT command received: " +
                                               std::string(contracts::mqtt::ToString(message.message_type))));
        }
        if (const auto* work_created =
                contracts::mqtt::GetPayload<contracts::mqtt::WorkCreatedPayload>(message)) {
            device_status->SetJobId(work_created->work_id);
            device_status->SetCurrentState("WORK_ASSIGNED");
        }
    });
    if (!mqtt_client.Start()) {
        if (log_uploader != nullptr) {
            log_uploader->Stop();
        }
        return 1;
    }

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    auto next_heartbeat = std::chrono::steady_clock::now();
    const std::string message_session_id = GenerateMessageSessionId();
    std::uint64_t message_sequence = 1;

    std::clog << "[device][INFO] device node started: id=" << device_id << "; role=" << contracts::ToString(role_)
              << '\n';

    while (stop_requested == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (mqtt_client.IsConnected() && now >= next_heartbeat) {
            static_cast<void>(mqtt_client.PublishHeartbeat(
                MakeMessageId(device_id, message_session_id, message_sequence++), CurrentIso8601Timestamp()));
            next_heartbeat = now + contracts::mqtt::kHeartbeatInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    mqtt_client.Stop();
    if (log_uploader != nullptr) {
        static_cast<void>(log_uploader->Append("device node stopped"));
        static_cast<void>(log_uploader->RotateNow());
        static_cast<void>(log_uploader->ProcessPendingOnce());
        log_uploader->Stop();
    }
    std::clog << "[device][INFO] device node stopped\n";
    return 0;
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
    std::cout << "device node scaffold: role=" << contracts::ToString(role_) << '\n';
    return 0;
#endif
}

}  // namespace logistics::device
