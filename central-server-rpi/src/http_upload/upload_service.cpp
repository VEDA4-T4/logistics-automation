#include "logistics/central_server/upload_service.hpp"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

namespace logistics::central_server {
namespace {

using contracts::http::UploadKind;

std::string GenerateUuid() {
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(random());
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

std::string ExtensionFor(std::string_view mime_type) {
    if (mime_type == "image/jpeg") {
        return ".jpg";
    }
    if (mime_type == "image/png") {
        return ".png";
    }
    if (mime_type == "application/gzip") {
        return ".gz";
    }
    if (mime_type == "application/zip") {
        return ".zip";
    }
    return ".log";
}

std::string KindText(UploadKind kind) {
    return kind == UploadKind::kImage ? "IMAGE" : "LOG";
}

std::filesystem::path KindDirectory(UploadKind kind) {
    return kind == UploadKind::kImage ? "images" : "logs";
}

std::int64_t CurrentUnixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool SameRequest(const UploadRequest& request, const Statement& row) {
    return row.ColumnText(3) == KindText(request.kind) && row.ColumnText(4) == request.device_id &&
           row.ColumnText(5) == request.work_id && row.ColumnText(6) == request.mime_type &&
           row.ColumnInt64(7) == static_cast<std::int64_t>(request.bytes.size()) && row.ColumnText(2) == request.sha256;
}

UploadResult LookupDuplicate(Database& database, const UploadRequest& request) {
    Statement statement;
    auto status = database.Prepare(
        "SELECT upload_id,relative_path,sha256,kind,device_id,COALESCE(work_id,''),mime_type,byte_size "
        "FROM http_upload WHERE idempotency_key=?",
        statement);
    if (!status.ok() || !(status = statement.Bind(1, request.message_id)).ok()) {
        return { UploadStatus::kStorageError, {}, {}, {}, status.message };
    }
    bool row = false;
    if (!(status = statement.Step(row)).ok()) {
        return { UploadStatus::kStorageError, {}, {}, {}, status.message };
    }
    if (!row) {
        return { UploadStatus::kNotFound, {}, {}, {}, "idempotency key not found" };
    }
    if (!SameRequest(request, statement)) {
        return { UploadStatus::kConflict, {}, {}, {}, "idempotency key was reused with different metadata" };
    }
    return { UploadStatus::kDuplicate, statement.ColumnText(0), "/uploads/" + statement.ColumnText(1),
             statement.ColumnText(2), "upload already exists" };
}

bool WorkExists(Database& database, std::string_view work_id, DatabaseStatus& status) {
    Statement statement;
    status = database.Prepare("SELECT 1 FROM product WHERE work_id=?", statement);
    if (!status.ok() || !(status = statement.Bind(1, work_id)).ok()) {
        return false;
    }
    bool row = false;
    status = statement.Step(row);
    return status.ok() && row;
}

}  // namespace

std::string Sha256Hex(const std::vector<std::uint8_t>& bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return {};
    }
    const bool succeeded = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                           EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
                           EVP_DigestFinal_ex(context, digest.data(), &digest_size) == 1;
    EVP_MD_CTX_free(context);
    if (!succeeded) {
        return {};
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

UploadService::UploadService(Database& database, std::filesystem::path root)
    : database_(database), root_(std::move(root)) {}

UploadResult UploadService::Store(const UploadRequest& request) {
    std::lock_guard lock(mutex_);
    const contracts::http::UploadMetadataView metadata{
        .kind = request.kind,
        .device_id = request.device_id,
        .work_id = request.work_id,
        .message_id = request.message_id,
        .captured_at = request.captured_at,
        .started_at = request.started_at,
        .ended_at = request.ended_at,
        .sha256 = request.sha256,
        .mime_type = request.mime_type,
        .byte_size = request.bytes.size(),
    };
    const auto validation = contracts::http::Validate(metadata);
    if (validation == contracts::http::ValidationError::kInvalidSize) {
        return { UploadStatus::kTooLarge, {}, {}, {}, "file size is outside the allowed range" };
    }
    if (validation == contracts::http::ValidationError::kUnsupportedMimeType) {
        return { UploadStatus::kUnsupportedMediaType, {}, {}, {}, "unsupported media type" };
    }
    if (validation != contracts::http::ValidationError::kNone) {
        return { UploadStatus::kInvalidRequest, {}, {}, {}, "invalid upload metadata" };
    }

    const std::string actual_checksum = Sha256Hex(request.bytes);
    if (actual_checksum.empty()) {
        return { UploadStatus::kStorageError, {}, {}, {}, "SHA-256 calculation failed" };
    }
    if (actual_checksum != request.sha256) {
        return { UploadStatus::kChecksumMismatch, {}, {}, actual_checksum, "SHA-256 mismatch" };
    }

    UploadResult duplicate = LookupDuplicate(database_, request);
    if (duplicate.status != UploadStatus::kNotFound) {
        return duplicate;
    }

    DatabaseStatus status;
    if (request.kind == UploadKind::kImage && !WorkExists(database_, request.work_id, status)) {
        return status.ok() ? UploadResult{ UploadStatus::kNotFound, {}, {}, {}, "workId does not exist" }
                           : UploadResult{ UploadStatus::kStorageError, {}, {}, {}, status.message };
    }

    const std::string upload_id = GenerateUuid();
    const std::filesystem::path relative_path =
        KindDirectory(request.kind) / (upload_id + ExtensionFor(request.mime_type));
    const std::filesystem::path destination = root_ / relative_path;
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return { UploadStatus::kStorageError, {}, {}, {}, "cannot create upload directory: " + error.message() };
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(request.bytes.data()),
                     static_cast<std::streamsize>(request.bytes.size()));
        if (!output) {
            std::filesystem::remove(temporary, error);
            return { UploadStatus::kStorageError, {}, {}, {}, "cannot write temporary upload" };
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return { UploadStatus::kStorageError, {}, {}, {}, "cannot commit upload file: " + error.message() };
    }

    Transaction transaction(database_);
    if (!transaction.status().ok()) {
        std::filesystem::remove(destination, error);
        return { UploadStatus::kStorageError, {}, {}, {}, transaction.status().message };
    }
    Statement insert;
    status = database_.Prepare(
        "INSERT INTO http_upload(upload_id,idempotency_key,kind,device_id,work_id,relative_path,mime_type,byte_size,"
        "sha256,captured_at,started_at,ended_at,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        insert);
    if (status.ok())
        status = insert.Bind(1, upload_id);
    if (status.ok())
        status = insert.Bind(2, request.message_id);
    if (status.ok())
        status = insert.Bind(3, KindText(request.kind));
    if (status.ok())
        status = insert.Bind(4, request.device_id);
    if (status.ok())
        status = request.work_id.empty() ? insert.BindNull(5) : insert.Bind(5, request.work_id);
    if (status.ok())
        status = insert.Bind(6, relative_path.generic_string());
    if (status.ok())
        status = insert.Bind(7, request.mime_type);
    if (status.ok())
        status = insert.Bind(8, static_cast<std::int64_t>(request.bytes.size()));
    if (status.ok())
        status = insert.Bind(9, actual_checksum);
    if (status.ok())
        status = request.captured_at.empty() ? insert.BindNull(10) : insert.Bind(10, request.captured_at);
    if (status.ok())
        status = request.started_at.empty() ? insert.BindNull(11) : insert.Bind(11, request.started_at);
    if (status.ok())
        status = request.ended_at.empty() ? insert.BindNull(12) : insert.Bind(12, request.ended_at);
    if (status.ok())
        status = insert.Bind(13, CurrentUnixMilliseconds());
    bool row = false;
    if (status.ok())
        status = insert.Step(row);
    if (status.ok())
        status = transaction.Commit();
    if (!status.ok()) {
        std::filesystem::remove(destination, error);
        return { UploadStatus::kStorageError, {}, {}, {}, status.message };
    }

    return { UploadStatus::kCreated, upload_id, "/uploads/" + relative_path.generic_string(), actual_checksum,
             "upload stored" };
}

}  // namespace logistics::central_server
