#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::device {

struct MqttPublishRecord final {
    std::string id;
    std::string topic;
    std::string payload;
    int qos{};
    bool retain{};
};

enum class MqttSpoolOverflowPolicy {
    kDiscardOldest,
    kRejectNew,
};

// QoS acknowledgements are broker-local, so records remain durable until the
// current connection reports PUBACK for their assigned message identifier.
class MqttPublishSpool final {
public:
    MqttPublishSpool(std::filesystem::path directory, std::size_t maximum_bytes, std::size_t maximum_records,
                     MqttSpoolOverflowPolicy overflow_policy = MqttSpoolOverflowPolicy::kDiscardOldest);

    [[nodiscard]] bool Start();
    [[nodiscard]] std::optional<MqttPublishRecord> Enqueue(std::string topic, std::string payload, int qos,
                                                           bool retain);
    [[nodiscard]] std::optional<MqttPublishRecord> Next() const;
    [[nodiscard]] bool Acknowledge(std::string_view id);
    [[nodiscard]] bool Quarantine(std::string_view id);
    [[nodiscard]] bool IsPending(std::string_view id) const;
    [[nodiscard]] std::size_t PendingCount() const;

private:
    std::filesystem::path directory_;
    std::size_t maximum_bytes_;
    std::size_t maximum_records_;
    MqttSpoolOverflowPolicy overflow_policy_;
    mutable std::mutex mutex_;
};

using VolatilePublisher = std::function<bool(std::string_view topic, std::string_view payload, int qos, bool retain)>;

[[nodiscard]] bool DeliverEvent(MqttPublishSpool& spool, contracts::mqtt::MessageType type, std::string topic,
                                std::string payload, const VolatilePublisher& publish_volatile);

}  // namespace logistics::device
