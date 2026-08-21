#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

struct WorkInvalidation final {
    std::string work_id;
    std::string message_id;
    std::string error_code;
    std::string reason;
    std::string cause;
    std::int64_t occurred_at_ms{};
};

[[nodiscard]] contracts::mqtt::MqttMessage MakeWorkInvalidationError(std::string_view source_id,
                                                                     const WorkInvalidation& invalidation,
                                                                     std::string timestamp);

}  // namespace logistics::central_server
