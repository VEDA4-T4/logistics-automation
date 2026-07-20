#include "logistics/central_server/application.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "logistics/central_server/database.hpp"
#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/mqtt_client.hpp"
#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/central_server/mqtt_handler.hpp"
#include "logistics/central_server/mqtt_transport.hpp"
#include "logistics/central_server/persistence.hpp"

namespace logistics::central_server {
namespace {

struct ServerConfig {
    DatabaseConfig database;
    StorageConfig storage;
};

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) {
    stop_requested = 1;
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseInteger(std::string_view text, int& output) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, output);

    return result.ec == std::errc{} &&
           result.ptr == end &&
           output >= 0;
}

bool ResolveConfigPath(
    int argc,
    char* argv[],
    std::filesystem::path& config_path
) {
    if (argc == 1) {
        if (const char* environment_path =
                std::getenv("LOGISTICS_CENTRAL_SERVER_CONFIG");
            environment_path != nullptr && *environment_path != '\0') {
            config_path = environment_path;
        } else {
            config_path = std::filesystem::path("config") / "server.ini";
        }

        return true;
    }

    if (argc == 2 && argv[1] != nullptr) {
        const std::string_view argument{ argv[1] };

        if (argument.starts_with("--config=")) {
            const auto value = argument.substr(
                std::string_view("--config=").size()
            );

            if (value.empty()) {
                return false;
            }

            config_path = std::string(value);
            return true;
        }

        if (!argument.empty() && !argument.starts_with("--")) {
            config_path = std::string(argument);
            return true;
        }

        return false;
    }

    if (argc == 3 &&
        argv[1] != nullptr &&
        argv[2] != nullptr &&
        std::string_view(argv[1]) == "--config" &&
        std::string_view(argv[2]).size() != 0) {
        config_path = argv[2];
        return true;
    }

    return false;
}

DatabaseStatus LoadServerConfig(
    const std::filesystem::path& path,
    ServerConfig& config
) {
    std::ifstream input(path);
    if (!input) {
        return {
            DatabaseStatusCode::kIoError,
            "cannot open config: " + path.string(),
        };
    }

    std::string section;
    std::string line;
    int line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        line = Trim(std::move(line));

        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }

        // MQTT와 device_registry 설정은 LoadMqttConfig가 처리합니다.
        if (section != "database" && section != "storage") {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            return {
                DatabaseStatusCode::kInvalidArgument,
                "invalid config line " + std::to_string(line_number),
            };
        }

        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));

        if (key.empty()) {
            return {
                DatabaseStatusCode::kInvalidArgument,
                "empty config key at line " +
                    std::to_string(line_number),
            };
        }

        if (section == "database" && key == "path") {
            config.database.path = value;
            continue;
        }

        if (section == "database" && key == "migration_dir") {
            config.database.migration_dir = value;
            continue;
        }

        if (section == "storage" && key == "image_root") {
            config.storage.image_root = value;
            continue;
        }

        // log_root는 현재 StorageConfig에 대응 필드가 없으므로
        // 다른 로깅 모듈에서 처리하도록 무시합니다.
        if (section == "storage" && key == "log_root") {
            continue;
        }

        const bool recognized_integer =
            (section == "database" && key == "busy_timeout_ms") ||
            (section == "storage" && key == "cleanup_interval_hours") ||
            (section == "storage" && key == "mqtt_retention_days") ||
            (section == "storage" &&
             key == "device_status_retention_days") ||
            (section == "storage" && key == "error_retention_days") ||
            (section == "storage" &&
             key == "security_retention_days") ||
            (section == "storage" && key == "image_retention_days");

        if (!recognized_integer) {
            // 다른 기능이 추가한 설정과 병합 가능하도록
            // 알 수 없는 키는 여기에서 거부하지 않습니다.
            continue;
        }

        int parsed = 0;
        if (!ParseInteger(value, parsed)) {
            return {
                DatabaseStatusCode::kInvalidArgument,
                "invalid integer at config line " +
                    std::to_string(line_number),
            };
        }

        if (section == "database") {
            config.database.busy_timeout_ms = parsed;
        } else if (key == "cleanup_interval_hours") {
            config.storage.cleanup_interval_hours = parsed;
        } else if (key == "mqtt_retention_days") {
            config.storage.mqtt_retention_days = parsed;
        } else if (key == "device_status_retention_days") {
            config.storage.device_status_retention_days = parsed;
        } else if (key == "error_retention_days") {
            config.storage.error_retention_days = parsed;
        } else if (key == "security_retention_days") {
            config.storage.security_retention_days = parsed;
        } else if (key == "image_retention_days") {
            config.storage.image_retention_days = parsed;
        }
    }

    return DatabaseStatus::Ok();
}

}  // namespace

int Application::Run(int argc, char* argv[]) {
    std::filesystem::path config_path;

    if (!ResolveConfigPath(argc, argv, config_path)) {
        std::cerr
            << "usage: logistics_central_server [server.ini]\n"
            << "   or: logistics_central_server --config <server.ini>\n"
            << "   or: logistics_central_server --config=<server.ini>\n";
        return 2;
    }

    MqttConfig mqtt_config;

    try {
        mqtt_config = LoadMqttConfig(config_path);
    } catch (const ConfigError& error) {
        std::cerr
            << "[server][ERROR] MQTT configuration failed: "
            << error.what()
            << '\n';
        return 2;
    }

    ServerConfig server_config;
    auto database_status = LoadServerConfig(
        config_path,
        server_config
    );

    if (!database_status.ok()) {
        std::cerr
            << "[server][ERROR] server configuration failed: "
            << database_status.message
            << '\n';
        return 2;
    }

    Database database;

    database_status = database.Open(server_config.database);
    if (!database_status.ok()) {
        std::cerr
            << "[server][ERROR] database open failed: "
            << database_status.message
            << '\n';
        return 3;
    }

    database_status = MigrationRunner::Apply(
        database,
        server_config.database.migration_dir
    );
    if (!database_status.ok()) {
        std::cerr
            << "[server][ERROR] database migration failed: "
            << database_status.message
            << '\n';
        return 3;
    }

    database_status = database.IntegrityCheck();
    if (!database_status.ok()) {
        std::cerr
            << "[server][ERROR] database integrity check failed: "
            << database_status.message
            << '\n';
        return 3;
    }

    RetentionService retention(database, server_config.storage);
    database_status = retention.RunOnce(
        CurrentUnixTimeMilliseconds()
    );

    if (!database_status.ok()) {
        std::cerr
            << "[server][ERROR] retention cleanup failed: "
            << database_status.message
            << '\n';
        return 4;
    }

    std::unique_ptr<DeviceManager> device_manager;

    try {
        device_manager = std::make_unique<DeviceManager>(
            mqtt_config.device_registry_path
        );
    } catch (const DeviceRegistryError& error) {
        std::cerr
            << "[server][ERROR] device registry failed: "
            << error.what()
            << '\n';
        return 5;
    }

    MqttHandler mqtt_handler(*device_manager);

    MqttClient mqtt_client(
        std::move(mqtt_config),
        CreateMosquittoTransport()
    );

    mqtt_client.SetMessageHandler(
        [&mqtt_handler](
            std::string_view topic,
            std::string_view payload
        ) {
            static_cast<void>(
                mqtt_handler.Handle(topic, payload)
            );
        }
    );

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (!mqtt_client.Start()) {
        std::cerr
            << "[server][ERROR] MQTT client startup failed\n";
        return 6;
    }

    std::clog
        << "[server][INFO] central server started; "
        << "registered devices="
        << device_manager->RegisteredDeviceCount()
        << "; database="
        << server_config.database.path.string()
        << '\n';

    while (stop_requested == 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(200)
        );
    }

    mqtt_client.Stop();

    std::clog
        << "[server][INFO] central server stopped\n";

    return 0;
}

}  // namespace logistics::central_server