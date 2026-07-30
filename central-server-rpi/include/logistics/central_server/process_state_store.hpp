#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "logistics/central_server/database.hpp"
#include "logistics/central_server/process_state_machine.hpp"

namespace logistics::central_server {

struct StoredProcessState final {
    ProcessSystemState system_state{ ProcessSystemState::kIdle };
    std::uint64_t message_sequence{};
    std::vector<WorkProcessSnapshot> works;
};

class ProcessStateStore final {
public:
    explicit ProcessStateStore(Database& database) : database_(database) {}

    [[nodiscard]] DatabaseStatus Load(std::optional<StoredProcessState>& output);
    [[nodiscard]] DatabaseStatus Save(ProcessSystemState system_state, std::uint64_t message_sequence,
                                      const std::vector<WorkProcessSnapshot>& works, std::int64_t updated_at_ms);

private:
    Database& database_;
};

}  // namespace logistics::central_server
