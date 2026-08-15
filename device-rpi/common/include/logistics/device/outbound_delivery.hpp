#pragma once

#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::device {

template <typename Publish, typename Queue>
[[nodiscard]] bool PublishOrQueueOutbound(const contracts::mqtt::MessageType type, const bool publish_immediately,
                                          Publish publish, Queue queue) {
    if (type == contracts::mqtt::MessageType::kSensorStatus) {
        return publish_immediately && publish();
    }
    if (publish_immediately && publish()) {
        return true;
    }
    return queue();
}

}  // namespace logistics::device
