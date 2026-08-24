#include "logistics/device/mqtt_publish_spool.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "logistics/contracts/mqtt_message.hpp"

namespace {

[[nodiscard]] std::filesystem::path TemporaryDirectory() {
    return std::filesystem::temp_directory_path() /
           ("logistics-mqtt-publish-spool-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void TestPubackControlsRemovalAndRestartReplay() {
    const auto directory = TemporaryDirectory();
    {
        logistics::device::MqttPublishSpool spool(directory, 4096, 4096);
        assert(spool.Start());
        const auto first = spool.Enqueue("device/PI-01/event", "first", 1, false);
        const auto second = spool.Enqueue("device/PI-01/response", "second", 1, false);
        assert(first.has_value() && second.has_value());
        // A successful mosquitto_publish call is not a PUBACK: the record remains durable.
        assert(spool.PendingCount() == 2);
        assert(spool.Next()->id == first->id);
        assert(spool.Acknowledge(first->id));
        assert(spool.Acknowledge(first->id));
        assert(spool.PendingCount() == 1);
    }
    {
        logistics::device::MqttPublishSpool restarted(directory, 4096, 4096);
        assert(restarted.Start());
        const auto replay = restarted.Next();
        assert(replay.has_value());
        assert(replay->payload == "second");
        assert(restarted.Acknowledge(replay->id));
        assert(restarted.PendingCount() == 0);
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestRestartDoesNotOverwriteUnacknowledgedRecord() {
    const auto directory = TemporaryDirectory();
    logistics::device::MqttPublishSpool first(directory, 4096, 4096);
    assert(first.Start());
    const auto original = first.Enqueue("device/PI-01/event", "original", 1, false);
    assert(original.has_value());

    logistics::device::MqttPublishSpool restarted(directory, 4096, 4096);
    assert(restarted.Start());
    const auto later = restarted.Enqueue("device/PI-01/event", "later", 1, false);
    assert(later.has_value());
    assert(later->id != original->id);
    assert(restarted.PendingCount() == 2);
    assert(restarted.Next()->payload == "original");

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestNumericOrderAndCorruptQuarantine() {
    const auto directory = TemporaryDirectory();
    logistics::device::MqttPublishSpool spool(directory, 65536, 4096);
    assert(spool.Start());
    for (int index = 0; index < 12; ++index) {
        assert(spool.Enqueue("device/PI-01/event", std::to_string(index), 1, false).has_value());
    }
    for (int index = 0; index < 12; ++index) {
        const auto next = spool.Next();
        assert(next.has_value());
        assert(next->payload == std::to_string(index));
        assert(spool.Acknowledge(next->id));
    }

    {
        std::ofstream corrupt(directory / "99.pending");
        corrupt << "{not-json";
    }
    {
        std::ofstream invalid(directory / "100.pending");
        invalid << R"({"id":"../../escape","topic":"device/PI-01/event","payload":"x","qos":1,"retain":false})";
    }
    {
        std::ofstream invalid(directory / "101.pending");
        invalid << R"({"id":"101","topic":"not/a/known/topic","payload":"x","qos":1,"retain":false})";
    }
    assert(!spool.Next().has_value());
    assert(std::filesystem::exists(directory / "99.corrupt"));
    assert(std::filesystem::exists(directory / "100.corrupt"));
    assert(std::filesystem::exists(directory / "101.corrupt"));
    assert(spool.PendingCount() == 0);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestSensorTelemetryNeverEntersDurableSpool() {
    namespace mqtt = logistics::contracts::mqtt;
    const auto directory = TemporaryDirectory();
    logistics::device::MqttPublishSpool spool(directory, 65536, 4096);
    assert(spool.Start());

    std::vector<std::string> volatile_payloads;
    const auto publish_volatile = [&volatile_payloads](std::string_view topic, std::string_view payload, int qos,
                                                       bool retain) {
        assert(topic == "device/PI-01/event");
        assert(qos == 0);
        assert(!retain);
        volatile_payloads.emplace_back(payload);
        return true;
    };

    assert(logistics::device::DeliverEvent(spool, mqtt::MessageType::kBoxDetected, "device/PI-01/event", "command",
                                           publish_volatile));
    for (int index = 0; index < 10'000; ++index) {
        assert(logistics::device::DeliverEvent(spool, mqtt::MessageType::kSensorStatus, "device/PI-01/event",
                                               "sensor-" + std::to_string(index), publish_volatile));
    }

    assert(volatile_payloads.size() == 10'000);
    assert(spool.PendingCount() == 1);
    assert(spool.Next()->payload == "command");

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestDroppedSensorIsNotReplayedAndNextSamplePublishes() {
    namespace mqtt = logistics::contracts::mqtt;
    const auto directory = TemporaryDirectory();
    logistics::device::MqttPublishSpool spool(directory, 4096, 4096);
    assert(spool.Start());

    std::vector<std::string> attempts;
    const auto publish_volatile = [&attempts](std::string_view, std::string_view payload, int qos, bool retain) {
        assert(qos == 0);
        assert(!retain);
        attempts.emplace_back(payload);
        return attempts.size() != 1;
    };

    assert(!logistics::device::DeliverEvent(spool, mqtt::MessageType::kSensorStatus, "device/PI-01/event", "stale",
                                            publish_volatile));
    assert(logistics::device::DeliverEvent(spool, mqtt::MessageType::kSensorStatus, "device/PI-01/event", "latest",
                                           publish_volatile));
    assert((attempts == std::vector<std::string>{ "stale", "latest" }));
    assert(spool.PendingCount() == 0);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestRecordLimitDiscardsOldestOutboundRecord() {
    const auto directory = TemporaryDirectory();
    logistics::device::MqttPublishSpool spool(directory, 65536, 2);
    assert(spool.Start());

    const auto first = spool.Enqueue("device/PI-01/event", "first", 1, false);
    const auto second = spool.Enqueue("device/PI-01/event", "second", 1, false);
    assert(first.has_value());
    assert(second.has_value());
    assert(spool.Enqueue("device/PI-01/event", "third", 1, false).has_value());
    assert(spool.PendingCount() == 2);
    assert(spool.Next()->payload == "second");

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestRestartRemovesIncompleteAndQuarantinedArtifacts() {
    const auto directory = TemporaryDirectory();
    std::filesystem::create_directories(directory);
    {
        std::ofstream temporary(directory / "1.tmp");
        temporary << "incomplete";
    }
    {
        std::ofstream corrupt(directory / "2.corrupt");
        corrupt << "invalid";
    }
    {
        std::ofstream rotated_corrupt(directory / "3.corrupt.4");
        rotated_corrupt << "invalid";
    }
    {
        std::ofstream unrelated(directory / "operator-note.txt");
        unrelated << "keep";
    }

    logistics::device::MqttPublishSpool spool(directory, 65536, 1);
    assert(spool.Start());
    assert(!std::filesystem::exists(directory / "1.tmp"));
    assert(!std::filesystem::exists(directory / "2.corrupt"));
    assert(!std::filesystem::exists(directory / "3.corrupt.4"));
    assert(std::filesystem::exists(directory / "operator-note.txt"));
    assert(spool.Enqueue("device/PI-01/event", "current", 1, false).has_value());

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestByteLimitDiscardsOldestOutboundRecord() {
    const auto directory = TemporaryDirectory();
    logistics::device::MqttPublishSpool spool(directory, 256, 4096);
    assert(spool.Start());
    assert(spool.Enqueue("device/PI-01/event", std::string(128, 'a'), 1, false).has_value());
    assert(spool.Enqueue("device/PI-01/event", std::string(128, 'b'), 1, false).has_value());
    assert(spool.PendingCount() == 1);
    assert(spool.Next()->payload == std::string(128, 'b'));

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

void TestInboundLimitRejectsNewCommandWithoutDiscardingOldest() {
    const auto directory = TemporaryDirectory();
    logistics::device::MqttPublishSpool spool(directory, 65536, 2,
                                              logistics::device::MqttSpoolOverflowPolicy::kRejectNew);
    assert(spool.Start());
    assert(spool.Enqueue("device/PI-01/command", "first", 1, false).has_value());
    assert(spool.Enqueue("device/PI-01/command", "second", 1, false).has_value());
    assert(!spool.Enqueue("device/PI-01/command", "third", 1, false).has_value());
    assert(spool.PendingCount() == 2);
    assert(spool.Next()->payload == "first");

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

}  // namespace

int main() {
    TestPubackControlsRemovalAndRestartReplay();
    TestRestartDoesNotOverwriteUnacknowledgedRecord();
    TestNumericOrderAndCorruptQuarantine();
    TestSensorTelemetryNeverEntersDurableSpool();
    TestDroppedSensorIsNotReplayedAndNextSamplePublishes();
    TestRecordLimitDiscardsOldestOutboundRecord();
    TestRestartRemovesIncompleteAndQuarantinedArtifacts();
    TestInboundLimitRejectsNewCommandWithoutDiscardingOldest();
    TestByteLimitDiscardsOldestOutboundRecord();
    return 0;
}
