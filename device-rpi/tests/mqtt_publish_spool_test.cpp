#include "logistics/device/mqtt_publish_spool.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

[[nodiscard]] std::filesystem::path TemporaryDirectory() {
    return std::filesystem::temp_directory_path() /
           ("logistics-mqtt-publish-spool-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void TestPubackControlsRemovalAndRestartReplay() {
    const auto directory = TemporaryDirectory();
    {
        logistics::device::MqttPublishSpool spool(directory, 4096);
        assert(spool.Start());
        const auto first = spool.Enqueue("device/PI-01/event", "first", 1, false);
        const auto second = spool.Enqueue("device/PI-01/response", "second", 1, false);
        assert(first.has_value() && second.has_value());
        // A successful mosquitto_publish call is not a PUBACK: the record remains durable.
        assert(spool.PendingCount() == 2);
        assert(spool.Next()->id == first->id);
        assert(spool.Acknowledge(first->id));
        assert(spool.PendingCount() == 1);
    }
    {
        logistics::device::MqttPublishSpool restarted(directory, 4096);
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
    logistics::device::MqttPublishSpool first(directory, 4096);
    assert(first.Start());
    const auto original = first.Enqueue("device/PI-01/event", "original", 1, false);
    assert(original.has_value());

    logistics::device::MqttPublishSpool restarted(directory, 4096);
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
    logistics::device::MqttPublishSpool spool(directory, 65536);
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

}  // namespace

int main() {
    TestPubackControlsRemovalAndRestartReplay();
    TestRestartDoesNotOverwriteUnacknowledgedRecord();
    TestNumericOrderAndCorruptQuarantine();
    return 0;
}
