#include "logistics/central_server/server_config.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {
namespace {

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

[[noreturn]] void ThrowLineError(const std::filesystem::path& path, std::size_t line_number, std::string_view detail) {
    throw ConfigError(path.string() + ":" + std::to_string(line_number) + ": " + std::string(detail));
}

[[nodiscard]] int ParseInteger(const std::filesystem::path& path, std::size_t line_number, std::string_view key,
                               std::string_view value, int minimum, int maximum) {
    int parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < minimum || parsed > maximum) {
        ThrowLineError(path, line_number,
                       std::string(key) + " is outside the allowed range [" + std::to_string(minimum) + ", " +
                           std::to_string(maximum) + "]");
    }
    return parsed;
}

[[nodiscard]] bool ParseBoolean(const std::filesystem::path& path, std::size_t line_number, std::string_view key,
                                std::string_view value) {
    std::string normalized(value);
    for (char& character : normalized) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (normalized == "true" || normalized == "yes" || normalized == "1") {
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "0") {
        return false;
    }
    ThrowLineError(path, line_number, std::string(key) + " must be true or false");
}

[[nodiscard]] std::filesystem::path ResolvePath(const std::filesystem::path& config_path, std::string_view value) {
    std::filesystem::path result(value);
    if (result.is_relative()) {
        result = config_path.parent_path() / result;
    }
    return result.lexically_normal();
}

void AssignPath(ServerConfig& config, const std::filesystem::path& path, std::size_t line_number,
                std::string_view section, std::string_view key, std::string_view value) {
    if (value.empty()) {
        if (section == "http" && key == "tls_certificate") {
            config.http.tls_certificate.clear();
            return;
        }
        if (section == "http" && key == "tls_private_key") {
            config.http.tls_private_key.clear();
            return;
        }
        ThrowLineError(path, line_number, std::string(key) + " must not be empty");
    }
    const auto resolved = ResolvePath(path, value);
    if (section == "database" && key == "path") {
        config.database.path = resolved;
    } else if (section == "database" && key == "migration_dir") {
        config.database.migration_dir = resolved;
    } else if (section == "storage" && key == "image_root") {
        config.storage.image_root = resolved;
    } else if (section == "http" && key == "tls_certificate") {
        config.http.tls_certificate = resolved;
    } else if (section == "http" && key == "tls_private_key") {
        config.http.tls_private_key = resolved;
    } else if (section == "http" && key == "upload_root") {
        config.http.upload_root = resolved;
    } else {
        ThrowLineError(path, line_number, "unknown [" + std::string(section) + "] setting: " + std::string(key));
    }
}

void AssignProcessId(ServerConfig& config, const std::filesystem::path& path, std::size_t line_number,
                     std::string_view key, std::string_view value) {
    if (!contracts::mqtt::IsValidTopicLevel(value)) {
        ThrowLineError(path, line_number, std::string(key) + " must be a valid MQTT topic level");
    }
    if (key == "server_id") {
        config.process.server_id = value;
    } else if (key == "input_device_id") {
        config.process.input_device_id = value;
    } else if (key == "vision_device_id") {
        config.process.vision_device_id = value;
    } else if (key == "gripper_device_id") {
        config.process.gripper_device_id = value;
    } else if (key == "sorting_device_id") {
        config.process.sorting_device_id = value;
    } else {
        config.process.line_tracer_device_id = value;
    }
}

void AssignValue(ServerConfig& config, const std::filesystem::path& path, std::size_t line_number,
                 std::string_view section, std::string_view key, std::string_view value) {
    const bool path_setting =
        (section == "database" && (key == "path" || key == "migration_dir")) ||
        (section == "storage" && key == "image_root") ||
        (section == "http" && (key == "tls_certificate" || key == "tls_private_key" || key == "upload_root"));
    if (path_setting) {
        AssignPath(config, path, line_number, section, key, value);
        return;
    }
    if (section == "database" && key == "busy_timeout_ms") {
        config.database.busy_timeout_ms = ParseInteger(path, line_number, key, value, 0, 600'000);
        return;
    }
    if (section == "storage" && key == "log_root") {
        // Kept for compatibility with configurations generated before log_root
        // was removed from StorageConfig.
        return;
    }
    if (section == "storage") {
        if (key == "cleanup_interval_hours") {
            config.storage.cleanup_interval_hours = ParseInteger(path, line_number, key, value, 1, 8'760);
        } else if (key == "mqtt_retention_days") {
            config.storage.mqtt_retention_days = ParseInteger(path, line_number, key, value, 1, 3'650);
        } else if (key == "device_status_retention_days") {
            config.storage.device_status_retention_days = ParseInteger(path, line_number, key, value, 1, 3'650);
        } else if (key == "error_retention_days") {
            config.storage.error_retention_days = ParseInteger(path, line_number, key, value, 1, 3'650);
        } else if (key == "security_retention_days") {
            config.storage.security_retention_days = ParseInteger(path, line_number, key, value, 1, 3'650);
        } else if (key == "image_retention_days") {
            config.storage.image_retention_days = ParseInteger(path, line_number, key, value, 1, 3'650);
        } else {
            ThrowLineError(path, line_number, "unknown [storage] setting: " + std::string(key));
        }
        return;
    }
    if (section == "http") {
        if (key == "enabled") {
            config.http.enabled = ParseBoolean(path, line_number, key, value);
        } else if (key == "port") {
            config.http.port = ParseInteger(path, line_number, key, value, 1, 65'535);
        } else if (key == "tls_enabled") {
            config.http.tls_enabled = ParseBoolean(path, line_number, key, value);
        } else if (key == "bearer_token") {
            config.http.bearer_token = value;
        } else {
            ThrowLineError(path, line_number, "unknown [http] setting: " + std::string(key));
        }
        return;
    }
    if (section == "routing" && key == "qt_client_id") {
        if (!contracts::mqtt::IsValidTopicLevel(value)) {
            ThrowLineError(path, line_number, "qt_client_id must be a valid MQTT topic level");
        }
        config.qt_client_id = value;
        return;
    }
    if (section == "process") {
        if (key == "enabled") {
            config.process.enabled = ParseBoolean(path, line_number, key, value);
        } else if (key == "server_id" || key == "input_device_id" || key == "vision_device_id" ||
                   key == "gripper_device_id" || key == "sorting_device_id" || key == "line_tracer_device_id") {
            AssignProcessId(config, path, line_number, key, value);
        } else {
            ThrowLineError(path, line_number, "unknown [process] setting: " + std::string(key));
        }
        return;
    }
    ThrowLineError(path, line_number, "unknown [" + std::string(section) + "] setting: " + std::string(key));
}

void ValidateConfig(const std::filesystem::path& path, const ServerConfig& config) {
    if (config.http.enabled && config.http.bearer_token.empty()) {
        throw ConfigError(path.string() + ": [http] bearer_token is required when HTTP upload is enabled");
    }
    if (config.http.enabled && config.http.tls_enabled &&
        (config.http.tls_certificate.empty() || config.http.tls_private_key.empty())) {
        throw ConfigError(path.string() +
                          ": [http] tls_certificate and tls_private_key are required when TLS is enabled");
    }
    if (config.http.enabled && config.http.tls_enabled) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(config.http.tls_certificate, error) || error) {
            throw ConfigError(path.string() +
                              ": [http] TLS certificate is not readable: " + config.http.tls_certificate.string());
        }
        error.clear();
        if (!std::filesystem::is_regular_file(config.http.tls_private_key, error) || error) {
            throw ConfigError(path.string() +
                              ": [http] TLS private key is not readable: " + config.http.tls_private_key.string());
        }
    }
    if (!config.process.IsValid()) {
        throw ConfigError(path.string() + ": [process] contains an invalid device identifier");
    }
}

}  // namespace

ServerConfig LoadServerConfig(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw ConfigError("unable to open server configuration: " + path.string());
    }

    ServerConfig config;
    std::string section;
    std::string line;
    std::size_t line_number{};
    std::unordered_set<std::string> assigned_keys;

    while (std::getline(input, line)) {
        ++line_number;
        std::string_view text = Trim(line);
        if (line_number == 1 && text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
            text = Trim(text);
        }
        if (text.empty() || text.front() == '#' || text.front() == ';') {
            continue;
        }
        if (text.front() == '[' && text.back() == ']') {
            section = std::string(Trim(text.substr(1, text.size() - 2)));
            if (section != "mqtt" && section != "device_registry" && section != "database" && section != "storage" &&
                section != "http" && section != "routing" && section != "process") {
                ThrowLineError(path, line_number, "unknown configuration section: " + section);
            }
            continue;
        }
        if (section.empty()) {
            ThrowLineError(path, line_number, "setting appears before a section header");
        }
        if (section == "mqtt" || section == "device_registry") {
            continue;
        }

        const auto delimiter = text.find('=');
        if (delimiter == std::string_view::npos) {
            ThrowLineError(path, line_number, "expected key=value");
        }
        const std::string_view key = Trim(text.substr(0, delimiter));
        const std::string_view value = Trim(text.substr(delimiter + 1));
        if (key.empty()) {
            ThrowLineError(path, line_number, "configuration key is empty");
        }
        const std::string identity = section + "." + std::string(key);
        if (!assigned_keys.emplace(identity).second) {
            ThrowLineError(path, line_number, "duplicate setting: " + identity);
        }
        AssignValue(config, path, line_number, section, key, value);
    }

    ValidateConfig(path, config);
    return config;
}

}  // namespace logistics::central_server
