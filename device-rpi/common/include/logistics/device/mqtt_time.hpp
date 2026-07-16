#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace logistics::device {

[[nodiscard]] std::string CurrentIso8601Timestamp();
[[nodiscard]] std::string GenerateMessageSessionId();
[[nodiscard]] std::string MakeMessageId(std::string_view source_id, std::string_view session_id,
                                        std::uint64_t sequence);

}  // namespace logistics::device
