#include "logistics/device/mqtt_node_config.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_set>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::device {
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
    throw NodeConfigError(path.string() + ":" + std::to_string(line_number) + ": " + std::string(detail));
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

void AssignDeviceValue(MqttNodeConfig& config, const std::filesystem::path& path, std::size_t line_number,
                       std::string_view key, std::string_view value) {
    if (key == "device_id") {
        config.device_id = value;
        return;
    }
    if (key == "node_name") {
        config.node_name = value;
        return;
    }
    if (key == "ip_address") {
        config.ip_address = value;
        return;
    }
    ThrowLineError(path, line_number, "unknown [device] setting: " + std::string(key));
}

void AssignMqttValue(MqttNodeConfig& config, const std::filesystem::path& path, std::size_t line_number,
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

void AssignLogUploadValue(MqttNodeConfig& config, const std::filesystem::path& path, std::size_t line_number,
                          std::string_view key, std::string_view value) {
    auto& upload = config.log_upload;
    if (key == "enabled") {
        config.log_upload_enabled = ParseBoolean(path, line_number, key, value);
    } else if (key == "endpoint_url") {
        upload.endpoint_url = value;
    } else if (key == "bearer_token") {
        upload.bearer_token = value;
    } else if (key == "ca_certificate") {
        upload.ca_certificate = value;
    } else if (key == "spool_directory") {
        upload.spool_directory = value;
    } else if (key == "rotate_bytes") {
        upload.rotate_bytes =
            ParseInteger<std::size_t>(path, line_number, key, value, 1, std::numeric_limits<std::size_t>::max());
    } else if (key == "rotate_interval_seconds") {
        upload.rotate_interval = std::chrono::seconds(
            ParseInteger<std::int64_t>(path, line_number, key, value, 1, std::numeric_limits<std::int64_t>::max()));
    } else if (key == "maximum_spool_bytes") {
        upload.maximum_spool_bytes =
            ParseInteger<std::size_t>(path, line_number, key, value, 1, std::numeric_limits<std::size_t>::max());
    } else if (key == "request_timeout_seconds") {
        upload.request_timeout = std::chrono::seconds(
            ParseInteger<std::int64_t>(path, line_number, key, value, 1, std::numeric_limits<std::int64_t>::max()));
    } else if (key == "maximum_attempts") {
        upload.maximum_attempts = ParseInteger<int>(path, line_number, key, value, 1, std::numeric_limits<int>::max());
    } else if (key == "initial_backoff_seconds") {
        upload.initial_backoff = std::chrono::seconds(
            ParseInteger<std::int64_t>(path, line_number, key, value, 1, std::numeric_limits<std::int64_t>::max()));
    } else if (key == "maximum_backoff_seconds") {
        upload.maximum_backoff = std::chrono::seconds(
            ParseInteger<std::int64_t>(path, line_number, key, value, 1, std::numeric_limits<std::int64_t>::max()));
    } else if (key == "allow_insecure_http") {
        upload.allow_insecure_http = ParseBoolean(path, line_number, key, value);
    } else {
        ThrowLineError(path, line_number, "unknown [log_upload] setting: " + std::string(key));
    }
}

void AssignImageUploadValue(MqttNodeConfig& config, const std::filesystem::path& path, std::size_t line_number,
                            std::string_view key, std::string_view value) {
    auto& upload = config.image_upload;
    if (key == "enabled") {
        config.image_upload_enabled = ParseBoolean(path, line_number, key, value);
    } else if (key == "endpoint_url") {
        upload.endpoint_url = value;
    } else if (key == "bearer_token") {
        upload.bearer_token = value;
    } else if (key == "ca_certificate") {
        upload.ca_certificate = value;
    } else if (key == "request_timeout_seconds") {
        upload.request_timeout = std::chrono::seconds(
            ParseInteger<std::int64_t>(path, line_number, key, value, 1, std::numeric_limits<std::int64_t>::max()));
    } else if (key == "maximum_attempts") {
        upload.maximum_attempts = ParseInteger<int>(path, line_number, key, value, 1, std::numeric_limits<int>::max());
    } else if (key == "initial_backoff_seconds") {
        upload.initial_backoff = std::chrono::seconds(
            ParseInteger<std::int64_t>(path, line_number, key, value, 1, std::numeric_limits<std::int64_t>::max()));
    } else if (key == "maximum_backoff_seconds") {
        upload.maximum_backoff = std::chrono::seconds(
            ParseInteger<std::int64_t>(path, line_number, key, value, 1, std::numeric_limits<std::int64_t>::max()));
    } else if (key == "allow_insecure_http") {
        upload.allow_insecure_http = ParseBoolean(path, line_number, key, value);
    } else {
        ThrowLineError(path, line_number, "unknown [image_upload] setting: " + std::string(key));
    }
}

}  // namespace

bool MqttNodeConfig::IsValid() const noexcept {
    const bool valid_log_upload =
        !log_upload_enabled ||
        (!log_upload.endpoint_url.empty() && !log_upload.spool_directory.empty() && log_upload.rotate_bytes != 0 &&
         log_upload.maximum_spool_bytes >= log_upload.rotate_bytes && log_upload.rotate_interval.count() > 0 &&
         log_upload.request_timeout.count() > 0 && log_upload.maximum_attempts > 0 &&
         log_upload.initial_backoff.count() > 0 && log_upload.maximum_backoff >= log_upload.initial_backoff);
    const bool valid_image_upload = !image_upload_enabled || image_upload.IsValid();
    return contracts::mqtt::IsValidTopicLevel(device_id) && !node_name.empty() && !ip_address.empty() &&
           !host.empty() && contracts::mqtt::IsValidTopicLevel(client_id) && port != 0 && keep_alive_seconds != 0 &&
           reconnect_min_delay_seconds != 0 && reconnect_max_delay_seconds >= reconnect_min_delay_seconds &&
           (password.empty() || !username.empty()) && valid_log_upload && valid_image_upload;
}

MqttNodeConfig LoadMqttNodeConfig(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw NodeConfigError("unable to open device configuration: " + path.string());
    }

    MqttNodeConfig config;
    std::string section;
    std::string line;
    std::size_t line_number{};
    bool found_device_section = false;
    bool found_mqtt_section = false;
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
            section = Trim(text.substr(1, text.size() - 2));
            found_device_section = found_device_section || section == "device";
            found_mqtt_section = found_mqtt_section || section == "mqtt";
            continue;
        }
        if (section != "device" && section != "mqtt" && section != "log_upload" && section != "image_upload") {
            continue;
        }

        const auto delimiter = text.find('=');
        if (delimiter == std::string_view::npos) {
            ThrowLineError(path, line_number, "expected key=value");
        }
        const std::string_view key = Trim(text.substr(0, delimiter));
        const std::string_view value = Trim(text.substr(delimiter + 1));
        const std::string scoped_key = section + "." + std::string(key);
        if (key.empty() || !assigned_keys.emplace(scoped_key).second) {
            ThrowLineError(path, line_number, "empty or duplicate setting: " + scoped_key);
        }

        if (section == "device") {
            AssignDeviceValue(config, path, line_number, key, value);
        } else if (section == "mqtt") {
            AssignMqttValue(config, path, line_number, key, value);
        } else if (section == "log_upload") {
            AssignLogUploadValue(config, path, line_number, key, value);
        } else {
            AssignImageUploadValue(config, path, line_number, key, value);
        }
    }

    config.log_upload.device_id = config.device_id;
    if (!found_device_section || !found_mqtt_section || !config.IsValid()) {
        throw NodeConfigError("invalid device MQTT configuration in " + path.string());
    }
    return config;
}

}  // namespace logistics::device
