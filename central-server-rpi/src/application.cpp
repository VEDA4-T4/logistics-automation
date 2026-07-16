#include "logistics/central_server/application.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "logistics/central_server/database.hpp"
#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/http_upload_server.hpp"
#include "logistics/central_server/persistence.hpp"

namespace logistics::central_server {
namespace {

struct ServerConfig {
    DatabaseConfig database;
    StorageConfig storage;
    HttpUploadServerConfig http;
};

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) {
    stop_requested = 1;
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseInteger(std::string_view text, int& output) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end && output >= 0;
}

bool ParseBoolean(std::string_view text, bool& output) {
    if (text == "true" || text == "1") {
        output = true;
        return true;
    }
    if (text == "false" || text == "0") {
        output = false;
        return true;
    }
    return false;
}

DatabaseStatus LoadConfig(const std::filesystem::path& path, bool required, ServerConfig& config) {
    std::ifstream input(path);
    if (!input) {
        return required ? DatabaseStatus{ DatabaseStatusCode::kIoError, "cannot open config: " + path.string() }
                        : DatabaseStatus::Ok();
    }
    std::string section;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            return { DatabaseStatusCode::kInvalidArgument, "invalid config line " + std::to_string(line_number) };
        }
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));
        if (section == "database" && key == "path")
            config.database.path = value;
        else if (section == "database" && key == "migration_dir")
            config.database.migration_dir = value;
        else if (section == "storage" && key == "image_root")
            config.storage.image_root = value;
        else if (section == "http" && key == "tls_certificate")
            config.http.tls_certificate = value;
        else if (section == "http" && key == "tls_private_key")
            config.http.tls_private_key = value;
        else if (section == "http" && key == "bearer_token")
            config.http.bearer_token = value;
        else if (section == "http" && key == "upload_root")
            config.http.upload_root = value;
        else if (section == "http" && (key == "enabled" || key == "tls_enabled")) {
            bool parsed = false;
            if (!ParseBoolean(value, parsed)) {
                return { DatabaseStatusCode::kInvalidArgument,
                         "invalid boolean at config line " + std::to_string(line_number) };
            }
            if (key == "enabled")
                config.http.enabled = parsed;
            else
                config.http.tls_enabled = parsed;
        } else {
            int parsed = 0;
            const bool recognized = (section == "database" && key == "busy_timeout_ms") ||
                                    (section == "storage" && key == "cleanup_interval_hours") ||
                                    (section == "storage" && key == "mqtt_retention_days") ||
                                    (section == "storage" && key == "device_status_retention_days") ||
                                    (section == "storage" && key == "error_retention_days") ||
                                    (section == "storage" && key == "security_retention_days") ||
                                    (section == "storage" && key == "image_retention_days") ||
                                    (section == "http" && key == "port");
            if (!recognized)
                continue;
            if (!ParseInteger(value, parsed)) {
                return { DatabaseStatusCode::kInvalidArgument,
                         "invalid integer at config line " + std::to_string(line_number) };
            }
            if (section == "database")
                config.database.busy_timeout_ms = parsed;
            else if (section == "http")
                config.http.port = parsed;
            else if (key == "cleanup_interval_hours")
                config.storage.cleanup_interval_hours = parsed;
            else if (key == "mqtt_retention_days")
                config.storage.mqtt_retention_days = parsed;
            else if (key == "device_status_retention_days")
                config.storage.device_status_retention_days = parsed;
            else if (key == "error_retention_days")
                config.storage.error_retention_days = parsed;
            else if (key == "security_retention_days")
                config.storage.security_retention_days = parsed;
            else
                config.storage.image_retention_days = parsed;
        }
    }
    return DatabaseStatus::Ok();
}

}  // namespace

int Application::Run(int argc, char** argv) {
    std::filesystem::path config_path{ "/etc/logistics/server.ini" };
    bool config_required = false;
    bool run_once = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{ argv[index] };
        if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
            config_required = true;
        } else if (argument.starts_with("--config=")) {
            config_path = std::string(argument.substr(9));
            config_required = true;
        } else if (argument == "--once") {
            run_once = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            return 2;
        }
    }
    ServerConfig config;
    auto status = LoadConfig(config_path, config_required, config);
    if (!status.ok()) {
        std::cerr << status.message << '\n';
        return 2;
    }
    Database database;
    if (!(status = database.Open(config.database)).ok() ||
        !(status = MigrationRunner::Apply(database, config.database.migration_dir)).ok() ||
        !(status = database.IntegrityCheck()).ok()) {
        std::cerr << "database initialization failed: " << status.message << '\n';
        return 3;
    }
    RetentionService retention(database, config.storage);
    if (!(status = retention.RunOnce(CurrentUnixTimeMilliseconds())).ok()) {
        std::cerr << "retention cleanup failed: " << status.message << '\n';
        return 4;
    }
    if (run_once || !config.http.enabled) {
        std::cout << "central server storage ready: registered devices=" << DeviceManager::RegisteredDeviceCount()
                  << '\n';
        return 0;
    }

    HttpUploadServer upload_server(database, config.http);
    if (!(status = upload_server.Start()).ok()) {
        std::cerr << "HTTP upload server failed: " << status.message << '\n';
        return 5;
    }
    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::cout << "central server ready: HTTP upload port=" << config.http.port
              << ", registered devices=" << DeviceManager::RegisteredDeviceCount() << '\n';
    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    upload_server.Stop();
    return 0;
}

}  // namespace logistics::central_server
