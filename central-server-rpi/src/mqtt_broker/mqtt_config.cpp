#include "logistics/central_server/mqtt_config.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>

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

template <typename Integer>
[[nodiscard]] Integer ParseInteger(const std::filesystem::path& path, std::size_t line_number, std::string_view key,
                                   std::string_view value, Integer minimum, Integer maximum) {
    unsigned long long parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < minimum || parsed > maximum) {
        ThrowLineError(path, line_number, std::string(key) + " is outside the allowed range");
    }
    return static_cast<Integer>(parsed);
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

void AssignMqttValue(MqttConfig& config, const std::filesystem::path& path, std::size_t line_number,
                     std::string_view key, std::string_view value) {
    if (key == "host") {
        config.host = value;
    } else if (key == "port") {
        config.port = ParseInteger<std::uint16_t>(path, line_number, key, value, 1, 65535);
    } else if (key == "client_id") {
        config.client_id = value;
    } else if (key == "username") {
        config.username = value;
    } else if (key == "password") {
        config.password = value;
    } else if (key == "keep_alive_seconds") {
        config.keep_alive_seconds = ParseInteger<std::uint16_t>(path, line_number, key, value, 1, 65535);
    } else if (key == "reconnect_min_delay_seconds") {
        config.reconnect_min_delay_seconds =
            ParseInteger<std::uint32_t>(path, line_number, key, value, 1, std::numeric_limits<std::uint32_t>::max());
    } else if (key == "reconnect_max_delay_seconds") {
        config.reconnect_max_delay_seconds =
            ParseInteger<std::uint32_t>(path, line_number, key, value, 1, std::numeric_limits<std::uint32_t>::max());
    } else if (key == "clean_session") {
        config.clean_session = ParseBoolean(path, line_number, key, value);
    } else {
        ThrowLineError(path, line_number, "unknown [mqtt] setting: " + std::string(key));
    }
}

}  // namespace

bool MqttConfig::IsValid() const noexcept {
    return !host.empty() && contracts::mqtt::IsValidTopicLevel(client_id) && port != 0 && keep_alive_seconds != 0 &&
           reconnect_min_delay_seconds != 0 && reconnect_max_delay_seconds >= reconnect_min_delay_seconds &&
           (password.empty() || !username.empty());
}

MqttConfig LoadMqttConfig(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw ConfigError("unable to open server configuration: " + path.string());
    }

    MqttConfig config;
    std::string section;
    std::string line;
    std::size_t line_number{};
    bool found_mqtt_section = false;
    std::unordered_set<std::string> assigned_keys;
    bool registry_path_assigned = false;

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
            section = Trim(text.substr(1, text.size() - 2));
            found_mqtt_section = found_mqtt_section || section == "mqtt";
            continue;
        }

        if (section != "mqtt" && section != "device_registry") {
            continue;
        }

        const auto delimiter = text.find('=');
        if (delimiter == std::string_view::npos) {
            ThrowLineError(path, line_number, "expected key=value in [mqtt] section");
        }
        const std::string_view key = Trim(text.substr(0, delimiter));
        const std::string_view value = Trim(text.substr(delimiter + 1));
        if (key.empty()) {
            ThrowLineError(path, line_number, "configuration key is empty");
        }
        if (section == "device_registry") {
            if (key != "path") {
                ThrowLineError(path, line_number, "unknown [device_registry] setting: " + std::string(key));
            }
            if (registry_path_assigned) {
                ThrowLineError(path, line_number, "duplicate [device_registry] setting: path");
            }
            config.device_registry_path = value;
            registry_path_assigned = true;
            continue;
        }
        if (!assigned_keys.emplace(key).second) {
            ThrowLineError(path, line_number, "duplicate [mqtt] setting: " + std::string(key));
        }
        AssignMqttValue(config, path, line_number, key, value);
    }

    if (!found_mqtt_section) {
        throw ConfigError("server configuration has no [mqtt] section: " + path.string());
    }
    if (!config.IsValid()) {
        throw ConfigError("invalid [mqtt] configuration in " + path.string() +
                          ": host/client_id are required, delays must be ordered, and password requires username");
    }
    if (config.device_registry_path.empty()) {
        config.device_registry_path = path.parent_path() / "devices.json";
    } else if (config.device_registry_path.is_relative()) {
        config.device_registry_path = path.parent_path() / config.device_registry_path;
    }
    return config;
}

}  // namespace logistics::central_server
