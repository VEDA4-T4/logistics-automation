#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace logistics::contracts::mqtt {

enum class TopicKind : std::uint8_t {
    kUnknown,
    kQtRequest,
    kQtResponse,
    kQtStatus,
    kQtEvent,
    kQtError,
    kDeviceRegister,
    kDeviceCommand,
    kDeviceResponse,
    kDeviceStatus,
    kDeviceEvent,
    kDeviceError,
    kDeviceHeartbeat,
    kSystemBroadcastCommand,
    kServerStatus,
    kServerHeartbeat,
};

struct ParsedTopic {
    TopicKind kind{ TopicKind::kUnknown };
    std::string_view endpoint_id;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return kind != TopicKind::kUnknown;
    }
};

inline constexpr std::string_view kSystemBroadcastCommandTopic = "system/broadcast/command";
inline constexpr std::string_view kServerStatusTopic = "server/status";
inline constexpr std::string_view kServerHeartbeatTopic = "server/heartbeat";

inline constexpr std::string_view kServerRequestSubscription = "server/request/+";
inline constexpr std::string_view kDeviceRegisterSubscription = "device/+/register";
inline constexpr std::string_view kDeviceResponseSubscription = "device/+/response";
inline constexpr std::string_view kDeviceStatusSubscription = "device/+/status";
inline constexpr std::string_view kDeviceEventSubscription = "device/+/event";
inline constexpr std::string_view kDeviceErrorSubscription = "device/+/error";
inline constexpr std::string_view kDeviceHeartbeatSubscription = "device/+/heartbeat";

[[nodiscard]] constexpr bool IsValidTopicLevel(std::string_view value) noexcept {
    return !value.empty() && value.find('/') == std::string_view::npos && value.find('+') == std::string_view::npos &&
           value.find('#') == std::string_view::npos && value.find('\0') == std::string_view::npos;
}

namespace detail {

[[nodiscard]] constexpr bool StartsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] constexpr bool EndsWith(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] inline std::string PrefixTopic(std::string_view prefix, std::string_view endpoint_id) {
    if (!IsValidTopicLevel(endpoint_id)) {
        throw std::invalid_argument("MQTT endpoint ID must be one non-wildcard topic level");
    }

    std::string topic;
    topic.reserve(prefix.size() + endpoint_id.size() + 1);
    topic.append(prefix);
    topic.push_back('/');
    topic.append(endpoint_id);
    return topic;
}

[[nodiscard]] inline std::string EndpointTopic(std::string_view prefix, std::string_view endpoint_id,
                                               std::string_view suffix) {
    if (!IsValidTopicLevel(endpoint_id)) {
        throw std::invalid_argument("MQTT endpoint ID must be one non-wildcard topic level");
    }

    std::string topic;
    topic.reserve(prefix.size() + endpoint_id.size() + suffix.size() + 2);
    topic.append(prefix);
    topic.push_back('/');
    topic.append(endpoint_id);
    topic.push_back('/');
    topic.append(suffix);
    return topic;
}

[[nodiscard]] constexpr ParsedTopic ParseEndpointTopic(std::string_view topic, std::string_view prefix,
                                                       std::string_view suffix, TopicKind kind) noexcept {
    const auto prefix_size = prefix.size() + 1;
    const auto suffix_size = suffix.size() + 1;
    if (topic.size() <= prefix_size + suffix_size || !StartsWith(topic, prefix) || topic[prefix.size()] != '/' ||
        !EndsWith(topic, suffix) || topic[topic.size() - suffix.size() - 1] != '/') {
        return {};
    }

    const auto endpoint = topic.substr(prefix_size, topic.size() - prefix_size - suffix_size);
    if (!IsValidTopicLevel(endpoint)) {
        return {};
    }
    return { .kind = kind, .endpoint_id = endpoint };
}

}  // namespace detail

[[nodiscard]] inline std::string QtRequestTopic(std::string_view client_id) {
    return detail::PrefixTopic("server/request", client_id);
}

[[nodiscard]] inline std::string QtResponseTopic(std::string_view client_id) {
    return detail::EndpointTopic("qt", client_id, "response");
}

[[nodiscard]] inline std::string QtStatusTopic(std::string_view client_id) {
    return detail::EndpointTopic("qt", client_id, "status");
}

[[nodiscard]] inline std::string QtEventTopic(std::string_view client_id) {
    return detail::EndpointTopic("qt", client_id, "event");
}

[[nodiscard]] inline std::string QtErrorTopic(std::string_view client_id) {
    return detail::EndpointTopic("qt", client_id, "error");
}

[[nodiscard]] inline std::string DeviceRegisterTopic(std::string_view device_id) {
    return detail::EndpointTopic("device", device_id, "register");
}

[[nodiscard]] inline std::string DeviceCommandTopic(std::string_view device_id) {
    return detail::EndpointTopic("device", device_id, "command");
}

[[nodiscard]] inline std::string DeviceResponseTopic(std::string_view device_id) {
    return detail::EndpointTopic("device", device_id, "response");
}

[[nodiscard]] inline std::string DeviceStatusTopic(std::string_view device_id) {
    return detail::EndpointTopic("device", device_id, "status");
}

[[nodiscard]] inline std::string DeviceEventTopic(std::string_view device_id) {
    return detail::EndpointTopic("device", device_id, "event");
}

[[nodiscard]] inline std::string DeviceErrorTopic(std::string_view device_id) {
    return detail::EndpointTopic("device", device_id, "error");
}

[[nodiscard]] inline std::string DeviceHeartbeatTopic(std::string_view device_id) {
    return detail::EndpointTopic("device", device_id, "heartbeat");
}

[[nodiscard]] constexpr ParsedTopic ParseTopic(std::string_view topic) noexcept {
    if (topic == kSystemBroadcastCommandTopic) {
        return { .kind = TopicKind::kSystemBroadcastCommand, .endpoint_id = {} };
    }
    if (topic == kServerStatusTopic) {
        return { .kind = TopicKind::kServerStatus, .endpoint_id = {} };
    }
    if (topic == kServerHeartbeatTopic) {
        return { .kind = TopicKind::kServerHeartbeat, .endpoint_id = {} };
    }

    struct TopicPattern {
        std::string_view prefix;
        std::string_view suffix;
        TopicKind kind;
    };
    constexpr std::array patterns = {
        TopicPattern{ .prefix = "server/request", .suffix = "", .kind = TopicKind::kQtRequest },
        TopicPattern{ .prefix = "qt", .suffix = "response", .kind = TopicKind::kQtResponse },
        TopicPattern{ .prefix = "qt", .suffix = "status", .kind = TopicKind::kQtStatus },
        TopicPattern{ .prefix = "qt", .suffix = "event", .kind = TopicKind::kQtEvent },
        TopicPattern{ .prefix = "qt", .suffix = "error", .kind = TopicKind::kQtError },
        TopicPattern{ .prefix = "device", .suffix = "register", .kind = TopicKind::kDeviceRegister },
        TopicPattern{ .prefix = "device", .suffix = "command", .kind = TopicKind::kDeviceCommand },
        TopicPattern{ .prefix = "device", .suffix = "response", .kind = TopicKind::kDeviceResponse },
        TopicPattern{ .prefix = "device", .suffix = "status", .kind = TopicKind::kDeviceStatus },
        TopicPattern{ .prefix = "device", .suffix = "event", .kind = TopicKind::kDeviceEvent },
        TopicPattern{ .prefix = "device", .suffix = "error", .kind = TopicKind::kDeviceError },
        TopicPattern{ .prefix = "device", .suffix = "heartbeat", .kind = TopicKind::kDeviceHeartbeat },
    };

    for (const auto& pattern : patterns) {
        if (pattern.kind == TopicKind::kQtRequest) {
            constexpr std::string_view request_prefix = "server/request/";
            if (detail::StartsWith(topic, request_prefix)) {
                const auto client_id = topic.substr(request_prefix.size());
                if (IsValidTopicLevel(client_id)) {
                    return { .kind = pattern.kind, .endpoint_id = client_id };
                }
            }
            continue;
        }

        const auto parsed = detail::ParseEndpointTopic(topic, pattern.prefix, pattern.suffix, pattern.kind);
        if (parsed.IsValid()) {
            return parsed;
        }
    }
    return {};
}

}  // namespace logistics::contracts::mqtt
