#include "logistics/device/outbound_delivery.hpp"

#include <cassert>
#include <deque>
#include <string>

namespace {

namespace mqtt = logistics::contracts::mqtt;
using logistics::device::PublishOrQueueOutbound;

void TestConnectedSensorPublishesWithoutQueueing() {
    int publish_calls{};
    int queue_calls{};

    assert(PublishOrQueueOutbound(
        mqtt::MessageType::kSensorStatus, true,
        [&] {
            ++publish_calls;
            return true;
        },
        [&] {
            ++queue_calls;
            return true;
        }));
    assert(publish_calls == 1);
    assert(queue_calls == 0);
}

void TestDroppedSensorNeverQueues() {
    int publish_calls{};
    int queue_calls{};

    assert(!PublishOrQueueOutbound(
        mqtt::MessageType::kSensorStatus, true,
        [&] {
            ++publish_calls;
            return false;
        },
        [&] {
            ++queue_calls;
            return true;
        }));
    assert(publish_calls == 1);
    assert(queue_calls == 0);
}

void TestDisconnectedSensorNeverQueues() {
    int publish_calls{};
    int queue_calls{};

    assert(!PublishOrQueueOutbound(
        mqtt::MessageType::kSensorStatus, false,
        [&] {
            ++publish_calls;
            return true;
        },
        [&] {
            ++queue_calls;
            return true;
        }));
    assert(publish_calls == 0);
    assert(queue_calls == 0);
}

void TestSensorBurstDoesNotBlockDurableQueue() {
    std::deque<std::string> outbox;
    const auto route = [&](mqtt::MessageType type, std::string payload) {
        return PublishOrQueueOutbound(
            type, true, [] { return false; },
            [&] {
                outbox.push_back(std::move(payload));
                return true;
            });
    };

    assert(route(mqtt::MessageType::kCommandResponse, "durable-first"));
    for (int index = 0; index < 10'000; ++index) {
        assert(!route(mqtt::MessageType::kSensorStatus, "sensor-" + std::to_string(index)));
    }
    assert(route(mqtt::MessageType::kWorkCompleted, "durable-second"));
    assert((outbox == std::deque<std::string>{ "durable-first", "durable-second" }));
}

void TestStatusKeepsQueueOnlyBehavior() {
    int publish_calls{};
    int queue_calls{};

    assert(PublishOrQueueOutbound(
        mqtt::MessageType::kDeviceStatus, false,
        [&] {
            ++publish_calls;
            return true;
        },
        [&] {
            ++queue_calls;
            return true;
        }));
    assert(publish_calls == 0);
    assert(queue_calls == 1);
}

}  // namespace

int main() {
    TestConnectedSensorPublishesWithoutQueueing();
    TestDroppedSensorNeverQueues();
    TestDisconnectedSensorNeverQueues();
    TestSensorBurstDoesNotBlockDurableQueue();
    TestStatusKeepsQueueOnlyBehavior();
    return 0;
}
