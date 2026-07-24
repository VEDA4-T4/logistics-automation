#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "logistics/central_server/database.hpp"

namespace logistics::central_server {

struct HistoryEntry final {
    std::string message_id;
    std::string event_type;
    std::string source_id;
    std::string state;
    std::string error_code;
    std::string severity;
    std::string message;
    std::string details_json;
    std::int64_t occurred_at_ms{};
};

class HistoryService final {
public:
    static constexpr std::size_t kDefaultLimit = 100;
    static constexpr std::size_t kMaximumLimit = 500;

    explicit HistoryService(Database& database) : database_(database) {}

    [[nodiscard]] DatabaseStatus FindByWorkId(std::string_view work_id, std::size_t limit,
                                              std::vector<HistoryEntry>& output);
    [[nodiscard]] DatabaseStatus FindByDeviceId(std::string_view device_id, std::size_t limit,
                                                std::vector<HistoryEntry>& output);

private:
    Database& database_;
};

}  // namespace logistics::central_server
