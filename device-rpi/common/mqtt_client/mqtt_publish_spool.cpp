#include "logistics/device/mqtt_publish_spool.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::device {
namespace {

constexpr std::string_view kPendingSuffix = ".pending";
constexpr std::string_view kCorruptSuffix = ".corrupt";
std::atomic_uint64_t sequence{};

[[nodiscard]] std::optional<std::uint64_t> NumericStem(const std::filesystem::path& path) {
    const auto stem = path.stem().string();
    if (stem.empty()) {
        return std::nullopt;
    }
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(stem.data(), stem.data() + stem.size(), value);
    if (error != std::errc{} || end != stem.data() + stem.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool IsSafeId(std::string_view id) {
    if (id.empty()) {
        return false;
    }
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), value);
    return error == std::errc{} && end == id.data() + id.size();
}

[[nodiscard]] bool IsDiscardableArtifact(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    const auto separator = name.find('.');
    if (separator == std::string::npos || !IsSafeId(std::string_view(name).substr(0, separator))) {
        return false;
    }
    const auto suffix = std::string_view(name).substr(separator);
    if (suffix == ".tmp" || suffix == kCorruptSuffix) {
        return true;
    }
    constexpr std::string_view kRotatedCorruptPrefix = ".corrupt.";
    return suffix.starts_with(kRotatedCorruptPrefix) && IsSafeId(suffix.substr(kRotatedCorruptPrefix.size()));
}

[[nodiscard]] std::vector<std::filesystem::path> Pending(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!error && entry.is_regular_file(error) && entry.path().extension() == kPendingSuffix) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        const auto left_number = NumericStem(left);
        const auto right_number = NumericStem(right);
        if (left_number.has_value() && right_number.has_value() && left_number != right_number) {
            return *left_number < *right_number;
        }
        return left.filename().string() < right.filename().string();
    });
    return files;
}

[[nodiscard]] std::optional<MqttPublishRecord> Read(const std::filesystem::path& path) {
    std::ifstream input(path);
    logistics::contracts::mqtt::Json value;
    try {
        input >> value;
        const auto id = value.at("id").get<std::string>();
        const auto topic = value.at("topic").get<std::string>();
        if (!IsSafeId(id) || id != path.stem().string() || !contracts::mqtt::ParseTopic(topic).IsValid()) {
            return std::nullopt;
        }
        return MqttPublishRecord{ .id = id,
                                  .topic = topic,
                                  .payload = value.at("payload").get<std::string>(),
                                  .qos = value.at("qos").get<int>(),
                                  .retain = value.at("retain").get<bool>() };
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

MqttPublishSpool::MqttPublishSpool(std::filesystem::path directory, const std::size_t maximum_bytes,
                                   const std::size_t maximum_records, const MqttSpoolOverflowPolicy overflow_policy)
    : directory_(std::move(directory)),
      maximum_bytes_(maximum_bytes),
      maximum_records_(maximum_records),
      overflow_policy_(overflow_policy) {}

bool MqttPublishSpool::Start() {
    std::lock_guard lock(mutex_);
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
        if (error) {
            return false;
        }
        if (entry.is_regular_file(error) && IsDiscardableArtifact(entry.path())) {
            std::filesystem::remove(entry.path(), error);
        }
        if (error) {
            return false;
        }
    }
    return !error;
}

std::optional<MqttPublishRecord> MqttPublishSpool::Enqueue(std::string topic, std::string payload, const int qos,
                                                           const bool retain) {
    std::lock_guard lock(mutex_);
    std::error_code directory_error;
    std::filesystem::create_directories(directory_, directory_error);
    if (topic.empty() || payload.empty() || qos != 1 || directory_error || maximum_records_ == 0U) {
        return std::nullopt;
    }
    auto pending_files = Pending(directory_);
    std::size_t total{};
    std::error_code error;
    for (const auto& path : pending_files) {
        error.clear();
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > std::numeric_limits<std::size_t>::max() - total) {
            return std::nullopt;
        }
        total += static_cast<std::size_t>(size);
    }
    std::string id;
    std::filesystem::path pending;
    std::uint64_t highest_id = sequence.load();
    for (const auto& path : pending_files) {
        if (const auto numeric_id = NumericStem(path); numeric_id.has_value() && *numeric_id > highest_id) {
            highest_id = *numeric_id;
        }
    }
    if (highest_id == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    sequence = highest_id;
    do {
        id = std::to_string(++sequence);
        if (id.empty()) {
            return std::nullopt;
        }
        pending = directory_ / (id + std::string(kPendingSuffix));
    } while (std::filesystem::exists(pending, error));
    const auto temporary = directory_ / (id + ".tmp");
    logistics::contracts::mqtt::Json value{
        { "id", id }, { "topic", topic }, { "payload", payload }, { "qos", qos }, { "retain", retain }
    };
    const std::string encoded = value.dump();
    if (encoded.size() > maximum_bytes_) {
        return std::nullopt;
    }
    while (overflow_policy_ == MqttSpoolOverflowPolicy::kDiscardOldest && !pending_files.empty() &&
           (pending_files.size() >= maximum_records_ || total > maximum_bytes_ - encoded.size())) {
        error.clear();
        const auto oldest_size = std::filesystem::file_size(pending_files.front(), error);
        if (error || oldest_size > total || !std::filesystem::remove(pending_files.front(), error) || error) {
            return std::nullopt;
        }
        total -= static_cast<std::size_t>(oldest_size);
        pending_files.erase(pending_files.begin());
    }
    if (pending_files.size() >= maximum_records_ || total > maximum_bytes_ - encoded.size()) {
        return std::nullopt;
    }
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            return std::nullopt;
        }
        output << encoded;
        output.flush();
        if (!output) {
            return std::nullopt;
        }
    }
    std::filesystem::rename(temporary, pending, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return std::nullopt;
    }
    return MqttPublishRecord{
        .id = id, .topic = std::move(topic), .payload = std::move(payload), .qos = qos, .retain = retain
    };
}

std::optional<MqttPublishRecord> MqttPublishSpool::Next() const {
    std::lock_guard lock(mutex_);
    for (const auto& path : Pending(directory_)) {
        if (const auto record = Read(path); record.has_value()) {
            return record;
        }
        std::error_code error;
        auto quarantined = path;
        quarantined.replace_extension(std::string(kCorruptSuffix));
        if (std::filesystem::exists(quarantined, error)) {
            quarantined += "." + std::to_string(++sequence);
        }
        std::filesystem::rename(path, quarantined, error);
    }
    return std::nullopt;
}

bool MqttPublishSpool::Acknowledge(std::string_view id) {
    std::lock_guard lock(mutex_);
    if (!IsSafeId(id)) {
        return false;
    }
    std::error_code error;
    // A PUBACK may be observed after an equivalent completion path already
    // removed the record. Absence is therefore an idempotent success; real
    // filesystem errors still halt the durable pump.
    static_cast<void>(std::filesystem::remove(directory_ / (std::string(id) + std::string(kPendingSuffix)), error));
    return !error;
}

bool MqttPublishSpool::Quarantine(std::string_view id) {
    std::lock_guard lock(mutex_);
    if (!IsSafeId(id)) {
        return false;
    }
    std::error_code error;
    const auto pending = directory_ / (std::string(id) + std::string(kPendingSuffix));
    if (!std::filesystem::exists(pending, error) || error) {
        return false;
    }
    auto quarantined = directory_ / (std::string(id) + std::string(kCorruptSuffix));
    if (std::filesystem::exists(quarantined, error)) {
        quarantined += "." + std::to_string(++sequence);
    }
    std::filesystem::rename(pending, quarantined, error);
    return !error;
}

bool MqttPublishSpool::IsPending(std::string_view id) const {
    std::lock_guard lock(mutex_);
    if (!IsSafeId(id)) {
        return false;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(directory_ / (std::string(id) + std::string(kPendingSuffix)), error);
    return exists && !error;
}

std::size_t MqttPublishSpool::PendingCount() const {
    std::lock_guard lock(mutex_);
    return Pending(directory_).size();
}

bool DeliverEvent(MqttPublishSpool& spool, const contracts::mqtt::MessageType type, std::string topic,
                  std::string payload, const VolatilePublisher& publish_volatile) {
    const auto policy = contracts::mqtt::PolicyFor(type);
    const int qos = static_cast<int>(policy.minimum_qos);
    if (contracts::mqtt::IsTransientTelemetry(type)) {
        return publish_volatile(topic, payload, qos, false);
    }
    return spool.Enqueue(std::move(topic), std::move(payload), qos, false).has_value();
}

}  // namespace logistics::device
