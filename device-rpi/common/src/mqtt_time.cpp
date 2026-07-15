#include "logistics/device/mqtt_time.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
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

std::string MakeMessageId(std::string_view source_id, std::uint64_t sequence) {
    return std::string(source_id) + "-MSG-" + std::to_string(sequence);
}

}  // namespace logistics::device
