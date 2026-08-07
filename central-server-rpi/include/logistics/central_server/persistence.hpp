#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "logistics/central_server/database.hpp"
#include "logistics/central_server/work_invalidation.hpp"
#include "logistics/contracts/mqtt_message.hpp"

namespace logistics::central_server {

struct StorageConfig {
    std::filesystem::path image_root{ "/var/lib/logistics/images" };
    int cleanup_interval_hours{ 24 };
    int mqtt_retention_days{ 30 };
    int device_status_retention_days{ 30 };
    int error_retention_days{ 180 };
    int security_retention_days{ 180 };
    int image_retention_days{ 30 };
    int upload_retention_days{ 30 };
};

struct TransportMetadata {
    std::string topic;
    int qos{ 1 };
    bool retained{ false };
    std::int64_t received_at_ms{};
    std::string source_address;
    std::string raw_payload;
};

struct EventPayload {
    std::optional<std::string> work_id;
    std::optional<std::string> barcode;
    std::optional<std::string> product_id;
    std::optional<std::string> product_name;
    std::optional<std::string> destination;
    std::optional<std::string> device_role;
    std::optional<std::string> connection_state;
    std::optional<std::string> process_state;
    std::optional<std::string> component_id;
    std::optional<std::string> error_code;
    std::optional<std::string> severity;
    std::optional<std::string> error_message;
    std::string details_json{ "{}" };
    std::vector<std::uint8_t> image_bytes;
    std::optional<std::string> image_mime_type;
    std::optional<std::int64_t> captured_at_ms;
    std::optional<std::string> image_id;
    std::optional<std::string> image_path;
    std::optional<std::string> image_checksum;
    std::optional<std::string> image_upload_status;
};

struct CatalogProduct {
    std::string barcode;
    std::string product_id;
    std::string product_name;
    std::string destination;
};

enum class PersistenceStatus : std::uint8_t { kStored, kDuplicate, kRetryableError, kPermanentError };

struct PersistenceResult {
    PersistenceStatus status{ PersistenceStatus::kPermanentError };
    std::string message;
    std::optional<std::string> work_id;

    [[nodiscard]] bool ok() const noexcept {
        return status == PersistenceStatus::kStored || status == PersistenceStatus::kDuplicate;
    }
};

struct StoredImage {
    std::filesystem::path relative_path;
    std::string sha256;
    std::int64_t byte_size{};
    bool created{ false };
};

class ImageStore final {
public:
    explicit ImageStore(std::filesystem::path root);
    [[nodiscard]] DatabaseStatus Store(std::string_view work_id, std::string_view mime_type,
                                       std::span<const std::uint8_t> bytes, std::int64_t captured_at_ms,
                                       StoredImage& output) const;
    [[nodiscard]] DatabaseStatus Remove(const std::filesystem::path& relative_path) const;

private:
    std::filesystem::path root_;
};

class EventRepository final {
public:
    explicit EventRepository(Database& database) : database_(database) {}
    [[nodiscard]] DatabaseStatus RecordReceived(const contracts::mqtt::EnvelopeView& envelope,
                                                const TransportMetadata& metadata, bool& duplicate);
    [[nodiscard]] DatabaseStatus MarkStored(std::string_view message_id, std::int64_t processed_at_ms);
    [[nodiscard]] DatabaseStatus MarkRejected(std::string_view message_id, std::string_view reason,
                                              std::int64_t processed_at_ms);

private:
    Database& database_;
};

class ProductRepository final {
public:
    explicit ProductRepository(Database& database) : database_(database) {}
    [[nodiscard]] DatabaseStatus Create(std::string_view work_id, std::int64_t now_ms);
    [[nodiscard]] DatabaseStatus MarkError(std::string_view work_id, std::int64_t now_ms);
    [[nodiscard]] DatabaseStatus ApplyEvent(std::string_view work_id, contracts::mqtt::MessageType type,
                                            const EventPayload& payload, std::int64_t now_ms);
    [[nodiscard]] DatabaseStatus AppendHistory(std::string_view work_id, std::string_view message_id,
                                               contracts::mqtt::MessageType type, std::string_view source_id,
                                               const EventPayload& payload, std::int64_t occurred_at_ms);

private:
    Database& database_;
};

class DeviceRepository final {
public:
    explicit DeviceRepository(Database& database) : database_(database) {}
    [[nodiscard]] DatabaseStatus AppendStatus(std::string_view message_id, std::string_view device_id,
                                              const EventPayload& payload, std::int64_t observed_at_ms);

private:
    Database& database_;
};

class LogRepository final {
public:
    explicit LogRepository(Database& database) : database_(database) {}
    [[nodiscard]] DatabaseStatus AppendError(std::string_view message_id, std::string_view source_id,
                                             const EventPayload& payload, std::int64_t occurred_at_ms);
    [[nodiscard]] DatabaseStatus AppendSecurity(std::string_view event_type, std::string_view actor_id,
                                                std::string_view source_address, std::string_view details_json,
                                                std::int64_t occurred_at_ms);

private:
    Database& database_;
};

class PersistenceService final {
public:
    PersistenceService(Database& database, StorageConfig storage_config);
    [[nodiscard]] PersistenceResult PersistValidatedEvent(const contracts::mqtt::EnvelopeView& envelope,
                                                          const EventPayload& payload,
                                                          const TransportMetadata& metadata);
    [[nodiscard]] DatabaseStatus RecordWorkInvalidation(const WorkInvalidation& invalidation);
    [[nodiscard]] DatabaseStatus FindActiveProductByBarcode(std::string_view barcode,
                                                            std::optional<CatalogProduct>& output);

private:
    Database& database_;
    ImageStore image_store_;
};

class RetentionService final {
public:
    RetentionService(Database& database, StorageConfig config, std::filesystem::path upload_root);
    [[nodiscard]] DatabaseStatus RunOnce(std::int64_t now_ms);

private:
    Database& database_;
    StorageConfig config_;
    ImageStore image_store_;
    ImageStore upload_store_;
};

[[nodiscard]] std::int64_t CurrentUnixTimeMilliseconds();

}  // namespace logistics::central_server
