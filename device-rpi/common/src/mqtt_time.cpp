#include "logistics/device/mqtt_time.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

namespace logistics::device {

std::string CurrentIso8601Timestamp() {
    const std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
    {
        static std::mutex time_mutex;
        std::lock_guard lock(time_mutex);
        const std::tm* converted = std::gmtime(&current_time);
        if (converted == nullptr) {
            return {};
        }
        utc_time = *converted;
    }

    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string GenerateMessageSessionId() {
    static std::atomic_uint64_t process_sequence{ 0 };
    const auto now =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    std::uint64_t entropy = static_cast<std::uint64_t>(now);
    try {
        std::random_device random;
        entropy ^= static_cast<std::uint64_t>(random()) << 32U;
        entropy ^= static_cast<std::uint64_t>(random());
    } catch (...) {
        entropy ^= static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << static_cast<std::uint64_t>(now) << '-' << std::setw(16)
           << entropy << '-' << std::setw(8) << process_sequence.fetch_add(1);
    return output.str();
}

std::string MakeMessageId(std::string_view source_id, std::string_view session_id, std::uint64_t sequence) {
    return std::string(source_id) + "-MSG-" + std::string(session_id) + '-' + std::to_string(sequence);
}

}  // namespace logistics::device
