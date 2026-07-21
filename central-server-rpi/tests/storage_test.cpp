#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "logistics/central_server/database.hpp"
#include "logistics/central_server/persistence.hpp"
#include "logistics/central_server/upload_service.hpp"

namespace {

std::int64_t Scalar(logistics::central_server::Database& database, std::string_view sql) {
    logistics::central_server::Statement statement;
    assert(database.Prepare(sql, statement).ok());
    bool row = false;
    assert(statement.Step(row).ok() && row);
    return statement.ColumnInt64(0);
}

logistics::contracts::mqtt::EnvelopeView Envelope(std::string_view id, logistics::contracts::mqtt::MessageType type,
                                                  std::string_view source = "PI-INPUT-01") {
    return { .protocol_version = logistics::contracts::mqtt::kCurrentProtocolVersion,
             .message_id = id,
             .message_type = type,
             .source_id = source,
             .timestamp = "2026-07-15T00:00:00Z",
             .data_json = "{}" };
}

logistics::central_server::TransportMetadata Metadata(std::int64_t time,
                                                      std::string topic = "device/PI-INPUT-01/event") {
    return { .topic = std::move(topic),
             .qos = 1,
             .retained = false,
             .received_at_ms = time,
             .source_address = "127.0.0.1",
             .raw_payload = "{}" };
}

}  // namespace

int main() {
    namespace server = logistics::central_server;
    namespace mqtt = logistics::contracts::mqtt;
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("logistics-storage-test-" + unique);
    std::filesystem::create_directories(root);

    server::Database database;
    server::DatabaseConfig database_config{ .path = root / "test.db",
                                            .migration_dir = LOGISTICS_TEST_MIGRATION_DIR,
                                            .busy_timeout_ms = 100 };
    assert(database.Open(database_config).ok());
    assert(server::MigrationRunner::Apply(database, database_config.migration_dir).ok());
    assert(server::MigrationRunner::Apply(database, database_config.migration_dir).ok());
    assert(database.IntegrityCheck().ok());
    assert(Scalar(database, "SELECT count(*) FROM schema_migrations") == 4);
    assert(Scalar(database,
                  "SELECT count(*) FROM product_catalog WHERE barcode='5901234123457' AND product_id='VEDA107' AND "
                  "product_name='VEDA107 기본 상품' AND destination='1' AND active=1") == 1);

    server::StorageConfig storage;
    storage.image_root = root / "images";
    server::PersistenceService persistence(database, storage);
    std::optional<server::CatalogProduct> catalog_product;
    assert(persistence.FindActiveProductByBarcode("5901234123457", catalog_product).ok());
    assert(catalog_product && catalog_product->product_id == "VEDA107" && catalog_product->destination == "1");
    assert(persistence.FindActiveProductByBarcode("0000000000000", catalog_product).ok());
    assert(!catalog_product);
    constexpr std::int64_t base_time = 1'700'000'000'000;

    server::Database lock_holder;
    assert(
        lock_holder
            .Open({ .path = root / "test.db", .migration_dir = database_config.migration_dir, .busy_timeout_ms = 100 })
            .ok());
    assert(lock_holder.Begin().ok());
    server::EventPayload busy_payload;
    busy_payload.device_role = "input";
    const auto busy_result =
        persistence.PersistValidatedEvent(Envelope("MSG-BUSY", mqtt::MessageType::kHeartbeat), busy_payload,
                                          Metadata(base_time, "device/PI-INPUT-01/heartbeat"));
    assert(busy_result.status == server::PersistenceStatus::kRetryableError);
    assert(lock_holder.Rollback().ok());

    server::EventPayload box;
    auto result = persistence.PersistValidatedEvent(Envelope("MSG-BOX-1", mqtt::MessageType::kBoxDetected), box,
                                                    Metadata(base_time));
    assert(result.status == server::PersistenceStatus::kStored && result.work_id);
    const std::string work_id = *result.work_id;
    assert(Scalar(database, "SELECT count(*) FROM product") == 1);
    assert(Scalar(database, "SELECT count(*) FROM work_history") == 1);

    result = persistence.PersistValidatedEvent(Envelope("MSG-BOX-1", mqtt::MessageType::kBoxDetected), box,
                                               Metadata(base_time + 1));
    assert(result.status == server::PersistenceStatus::kDuplicate);
    assert(Scalar(database, "SELECT duplicate_count FROM mqtt_event_log WHERE message_id='MSG-BOX-1'") == 1);
    assert(Scalar(database, "SELECT count(*) FROM product") == 1);

    server::EventPayload barcode;
    barcode.work_id = work_id;
    barcode.barcode = "8801234567890";
    result = persistence.PersistValidatedEvent(Envelope("MSG-BARCODE-1", mqtt::MessageType::kBarcodeDetected), barcode,
                                               Metadata(base_time + 2));
    assert(result.status == server::PersistenceStatus::kStored);

    server::EventPayload product_info;
    product_info.work_id = work_id;
    product_info.barcode = "5901234123457";
    product_info.product_id = "VEDA107";
    product_info.product_name = "VEDA107 기본 상품";
    product_info.destination = "1";
    result = persistence.PersistValidatedEvent(Envelope("MSG-PRODUCT-1", mqtt::MessageType::kProductInfo), product_info,
                                               Metadata(base_time + 3));
    assert(result.status == server::PersistenceStatus::kStored);
    assert(Scalar(database,
                  "SELECT count(*) FROM product WHERE barcode='5901234123457' AND product_id='VEDA107' AND "
                  "product_name='VEDA107 기본 상품' AND destination='1'") == 1);

    server::EventPayload missing_work;
    missing_work.work_id = "00000000-0000-4000-8000-000000000000";
    missing_work.destination = "A-01";
    result = persistence.PersistValidatedEvent(Envelope("MSG-BAD-WORK", mqtt::MessageType::kDestinationSet),
                                               missing_work, Metadata(base_time + 4));
    assert(result.status == server::PersistenceStatus::kPermanentError);
    assert(
        Scalar(database,
               "SELECT count(*) FROM mqtt_event_log WHERE message_id='MSG-BAD-WORK' AND processing_state='REJECTED'") ==
        1);

    server::EventPayload barcode_failure;
    barcode_failure.work_id = work_id;
    barcode_failure.process_state = "FAILED";
    result = persistence.PersistValidatedEvent(Envelope("MSG-BARCODE-FAIL", mqtt::MessageType::kBarcodeDetected),
                                               barcode_failure, Metadata(base_time + 5));
    assert(result.status == server::PersistenceStatus::kStored);
    assert(Scalar(database, "SELECT count(*) FROM product WHERE barcode='8801234567890'") == 1);

    server::UploadService upload_service(database, root / "uploads");
    const std::vector<std::uint8_t> image_bytes{ 0xff, 0xd8, 0xff, 0xd9 };
    const server::UploadRequest image_upload{
        .kind = logistics::contracts::http::UploadKind::kImage,
        .device_id = "PI-VISION-01",
        .work_id = work_id,
        .message_id = "UPLOAD-IMAGE-1",
        .captured_at = "2026-07-15T00:00:00Z",
        .started_at = {},
        .ended_at = {},
        .sha256 = server::Sha256Hex(image_bytes),
        .mime_type = "image/jpeg",
        .bytes = image_bytes,
    };
    const auto uploaded = upload_service.Store(image_upload);
    assert(uploaded.status == server::UploadStatus::kCreated);

    server::EventPayload image;
    image.work_id = work_id;
    image.image_id = uploaded.upload_id;
    image.image_path = uploaded.path;
    image.image_checksum = uploaded.checksum;
    image.image_upload_status = "UPLOADED";
    result =
        persistence.PersistValidatedEvent(Envelope("MSG-IMAGE-1", mqtt::MessageType::kProductImage, "PI-VISION-01"),
                                          image, Metadata(base_time + 6, "device/PI-VISION-01/event"));
    assert(result.status == server::PersistenceStatus::kStored);
    assert(Scalar(database, "SELECT count(*) FROM http_upload WHERE kind='IMAGE'") == 1);
    assert(Scalar(database, "SELECT count(*) FROM product WHERE lifecycle_state='IMAGED'") == 1);

    server::EventPayload bad_image = image;
    bad_image.work_id = "00000000-0000-4000-8000-000000000001";
    result =
        persistence.PersistValidatedEvent(Envelope("MSG-IMAGE-BAD", mqtt::MessageType::kProductImage, "PI-VISION-01"),
                                          bad_image, Metadata(base_time + 4, "device/PI-VISION-01/event"));
    assert(result.status == server::PersistenceStatus::kPermanentError);
    assert(Scalar(database, "SELECT count(*) FROM http_upload WHERE kind='IMAGE'") == 1);

    server::EventPayload completed;
    completed.work_id = work_id;
    result = persistence.PersistValidatedEvent(Envelope("MSG-DONE-1", mqtt::MessageType::kWorkCompleted), completed,
                                               Metadata(base_time + 5));
    assert(result.status == server::PersistenceStatus::kStored);
    assert(Scalar(database, "SELECT count(*) FROM product WHERE lifecycle_state='COMPLETED'") == 1);

    server::EventPayload device;
    device.device_role = "input";
    device.connection_state = "ONLINE";
    device.process_state = "AWAITING_WORK_ID";
    result = persistence.PersistValidatedEvent(Envelope("MSG-STATUS-OLD", mqtt::MessageType::kDeviceStatus), device,
                                               Metadata(base_time, "device/PI-INPUT-01/status"));
    assert(result.ok());
    device.connection_state = "OFFLINE";
    device.process_state = "VISION_REPORTED";
    result = persistence.PersistValidatedEvent(Envelope("MSG-STATUS-NEW", mqtt::MessageType::kDeviceStatus), device,
                                               Metadata(base_time + 40 * 86'400'000LL, "device/PI-INPUT-01/status"));
    assert(result.ok());
    assert(Scalar(database, "SELECT count(*) FROM device_status WHERE process_state='VISION_REPORTED'") == 1);

    server::RetentionService retention(database, storage);
    assert(retention.RunOnce(base_time + 40 * 86'400'000LL).ok());
    assert(Scalar(database, "SELECT count(*) FROM device_status WHERE device_id='PI-INPUT-01'") == 1);
    assert(Scalar(database, "SELECT count(*) FROM image_file") == 0);

    const auto migration_copy = root / "migrations";
    std::filesystem::create_directories(migration_copy);
    std::filesystem::copy_file(database_config.migration_dir / "001_initial.sql", migration_copy / "001_initial.sql");
    server::Database checksum_database;
    assert(checksum_database.Open({ .path = root / "checksum.db", .migration_dir = migration_copy }).ok());
    assert(server::MigrationRunner::Apply(checksum_database, migration_copy).ok());
    {
        std::ofstream changed(migration_copy / "001_initial.sql", std::ios::app);
        changed << "\n-- changed\n";
    }
    assert(server::MigrationRunner::Apply(checksum_database, migration_copy).code ==
           server::DatabaseStatusCode::kMigrationError);

    const auto gap_dir = root / "gap-migrations";
    std::filesystem::create_directories(gap_dir);
    std::filesystem::copy_file(database_config.migration_dir / "001_initial.sql", gap_dir / "002_gap.sql");
    server::Database gap_database;
    assert(gap_database.Open({ .path = root / "gap.db", .migration_dir = gap_dir }).ok());
    assert(server::MigrationRunner::Apply(gap_database, gap_dir).code == server::DatabaseStatusCode::kMigrationError);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return 0;
}
