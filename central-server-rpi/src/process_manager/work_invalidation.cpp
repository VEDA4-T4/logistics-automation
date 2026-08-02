#include "logistics/central_server/work_invalidation.hpp"

#include <optional>
#include <utility>

namespace logistics::central_server {

contracts::mqtt::MqttMessage MakeWorkInvalidationError(std::string_view source_id, const WorkInvalidation& invalidation,
                                                       std::string timestamp) {
    return {
        .protocol_version = std::string(contracts::mqtt::kCurrentProtocolVersion),
        .message_id = invalidation.message_id,
        .message_type = contracts::mqtt::MessageType::kErrorOccurred,
        .source_id = std::string(source_id),
        .timestamp = std::move(timestamp),
        .data =
            contracts::mqtt::ErrorOccurredPayload{
                .job_id = invalidation.work_id,
                .error_code = invalidation.error_code,
                .error_level = "ERROR",
                .current_state = "RECALIBRATION_REQUIRED",
                .message = invalidation.reason,
                .distance = std::nullopt,
            },
    };
}

}  // namespace logistics::central_server
