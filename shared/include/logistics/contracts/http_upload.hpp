#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "logistics/contracts/identifier.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::contracts::http {

inline constexpr std::string_view kImageUploadEndpoint = "/api/v1/uploads/images";
inline constexpr std::string_view kLogUploadEndpoint = "/api/v1/uploads/logs";

inline constexpr std::string_view kIdempotencyHeader = "Idempotency-Key";
inline constexpr std::string_view kAuthorizationHeader = "Authorization";
inline constexpr std::string_view kBearerPrefix = "Bearer ";

inline constexpr std::string_view kFileField = "file";
inline constexpr std::string_view kDeviceIdField = "deviceId";
inline constexpr std::string_view kWorkIdField = "workId";
inline constexpr std::string_view kMessageIdField = "messageId";
inline constexpr std::string_view kCapturedAtField = "capturedAt";
inline constexpr std::string_view kStartedAtField = "startedAt";
inline constexpr std::string_view kEndedAtField = "endedAt";
inline constexpr std::string_view kChecksumField = "sha256";
inline constexpr std::string_view kByteSizeField = "byteSize";

inline constexpr std::string_view kUploadIdResponseField = "uploadId";
inline constexpr std::string_view kPathResponseField = "path";
inline constexpr std::string_view kChecksumResponseField = "checksum";
inline constexpr std::string_view kDuplicateResponseField = "duplicate";

inline constexpr std::size_t kMaximumImageBytes = 10U * 1024U * 1024U;
inline constexpr std::size_t kMaximumLogBytes = 25U * 1024U * 1024U;
inline constexpr int kDefaultRequestTimeoutSeconds = 30;
inline constexpr int kDefaultMaximumRetries = 5;
inline constexpr int kDefaultInitialBackoffSeconds = 1;
inline constexpr int kDefaultMaximumBackoffSeconds = 60;

enum class UploadKind : std::uint8_t { kImage, kLog };

enum class ValidationError : std::uint8_t {
    kNone,
    kInvalidDeviceId,
    kInvalidMessageId,
    kMissingWorkId,
    kMissingTimestamp,
    kInvalidChecksum,
    kUnsupportedMimeType,
    kInvalidSize,
};

struct UploadMetadataView {
    UploadKind kind{ UploadKind::kImage };
    std::string_view device_id;
    std::string_view work_id;
    std::string_view message_id;
    std::string_view captured_at;
    std::string_view started_at;
    std::string_view ended_at;
    std::string_view sha256;
    std::string_view mime_type;
    std::size_t byte_size{};
};

[[nodiscard]] constexpr std::size_t MaximumBytes(UploadKind kind) noexcept {
    return kind == UploadKind::kImage ? kMaximumImageBytes : kMaximumLogBytes;
}

[[nodiscard]] constexpr bool IsSupportedMimeType(UploadKind kind, std::string_view mime_type) noexcept {
    if (kind == UploadKind::kImage) {
        return mime_type == "image/jpeg" || mime_type == "image/png";
    }
    return mime_type == "text/plain" || mime_type == "application/gzip" || mime_type == "application/zip";
}

[[nodiscard]] inline bool IsSha256(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] inline ValidationError Validate(const UploadMetadataView& metadata) noexcept {
    if (!mqtt::IsValidTopicLevel(metadata.device_id)) {
        return ValidationError::kInvalidDeviceId;
    }
    if (!mqtt::IsValidTopicLevel(metadata.message_id)) {
        return ValidationError::kInvalidMessageId;
    }
    if (metadata.kind == UploadKind::kImage && !IsValidUuid(metadata.work_id)) {
        return ValidationError::kMissingWorkId;
    }
    if ((metadata.kind == UploadKind::kImage && metadata.captured_at.empty()) ||
        (metadata.kind == UploadKind::kLog && (metadata.started_at.empty() || metadata.ended_at.empty()))) {
        return ValidationError::kMissingTimestamp;
    }
    if (!IsSha256(metadata.sha256)) {
        return ValidationError::kInvalidChecksum;
    }
    if (!IsSupportedMimeType(metadata.kind, metadata.mime_type)) {
        return ValidationError::kUnsupportedMimeType;
    }
    if (metadata.byte_size == 0 || metadata.byte_size > MaximumBytes(metadata.kind)) {
        return ValidationError::kInvalidSize;
    }
    return ValidationError::kNone;
}

}  // namespace logistics::contracts::http
