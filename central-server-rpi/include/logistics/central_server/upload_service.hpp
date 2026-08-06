#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "logistics/central_server/database.hpp"
#include "logistics/contracts/http_upload.hpp"

namespace logistics::central_server {

struct UploadRequest {
    contracts::http::UploadKind kind{ contracts::http::UploadKind::kImage };
    std::string device_id;
    std::string work_id;
    std::string message_id;
    std::string captured_at;
    std::string started_at;
    std::string ended_at;
    std::string sha256;
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
};

enum class UploadStatus : std::uint8_t {
    kCreated,
    kDuplicate,
    kInvalidRequest,
    kNotFound,
    kConflict,
    kTooLarge,
    kUnsupportedMediaType,
    kChecksumMismatch,
    kStorageError,
};

struct UploadResult {
    UploadStatus status{ UploadStatus::kInvalidRequest };
    std::string upload_id;
    std::string path;
    std::string checksum;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return status == UploadStatus::kCreated || status == UploadStatus::kDuplicate;
    }
};

class UploadService final {
public:
    UploadService(Database& database, std::filesystem::path root);
    [[nodiscard]] UploadResult Store(const UploadRequest& request);

private:
    Database& database_;
    std::filesystem::path root_;
    std::mutex mutex_;
};

[[nodiscard]] std::string Sha256Hex(const std::vector<std::uint8_t>& bytes);

}  // namespace logistics::central_server
