#include "logistics/central_server/upload_service.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "logistics/central_server/database.hpp"

namespace {

std::int64_t Scalar(logistics::central_server::Database& database, std::string_view sql) {
    logistics::central_server::Statement statement;
    assert(database.Prepare(sql, statement).ok());
    bool row = false;
    assert(statement.Step(row).ok() && row);
    return statement.ColumnInt64(0);
}

}  // namespace

int main() {
    namespace contract = logistics::contracts::http;
    namespace server = logistics::central_server;

    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("logistics-upload-test-" + unique);
    std::filesystem::create_directories(root);

    server::Database database;
    const server::DatabaseConfig config{ .path = root / "test.db",
                                         .migration_dir = LOGISTICS_TEST_MIGRATION_DIR,
                                         .busy_timeout_ms = 100 };
    assert(database.Open(config).ok());
    assert(server::MigrationRunner::Apply(database, config.migration_dir).ok());
    assert(database
               .Execute("INSERT INTO product(work_id,lifecycle_state,created_at_ms,updated_at_ms) "
                        "VALUES('WORK-001','DETECTED',1,1)")
               .ok());

    server::UploadService service(database, root / "uploads");
    const std::vector<std::uint8_t> image_bytes{ 0xff, 0xd8, 0xff, 0xd9 };
    server::UploadRequest image{ .kind = contract::UploadKind::kImage,
                                 .device_id = "PI-VISION-01",
                                 .work_id = "WORK-001",
                                 .message_id = "IMAGE-001",
                                 .captured_at = "2026-07-16T10:00:00Z",
                                 .sha256 = server::Sha256Hex(image_bytes),
                                 .mime_type = "image/jpeg",
                                 .bytes = image_bytes };

    auto result = service.Store(image);
    assert(result.status == server::UploadStatus::kCreated);
    assert(result.checksum == image.sha256);
    assert(std::filesystem::exists(root / "uploads" / result.path.substr(std::string("/uploads/").size())));
    assert(Scalar(database, "SELECT count(*) FROM http_upload") == 1);

    result = service.Store(image);
    assert(result.status == server::UploadStatus::kDuplicate);
    assert(Scalar(database, "SELECT count(*) FROM http_upload") == 1);

    auto conflict = image;
    conflict.bytes.push_back(0U);
    conflict.sha256 = server::Sha256Hex(conflict.bytes);
    assert(service.Store(conflict).status == server::UploadStatus::kConflict);

    auto checksum_mismatch = image;
    checksum_mismatch.message_id = "IMAGE-002";
    checksum_mismatch.sha256.assign(64, '0');
    assert(service.Store(checksum_mismatch).status == server::UploadStatus::kChecksumMismatch);

    auto missing_work = image;
    missing_work.message_id = "IMAGE-003";
    missing_work.work_id = "WORK-MISSING";
    assert(service.Store(missing_work).status == server::UploadStatus::kNotFound);

    const std::vector<std::uint8_t> log_bytes{ 'o', 'k', '\n' };
    const server::UploadRequest log{ .kind = contract::UploadKind::kLog,
                                     .device_id = "PI-VISION-01",
                                     .message_id = "LOG-001",
                                     .started_at = "2026-07-16T09:00:00Z",
                                     .ended_at = "2026-07-16T10:00:00Z",
                                     .sha256 = server::Sha256Hex(log_bytes),
                                     .mime_type = "text/plain",
                                     .bytes = log_bytes };
    assert(service.Store(log).status == server::UploadStatus::kCreated);
    assert(Scalar(database, "SELECT count(*) FROM http_upload") == 2);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return 0;
}
