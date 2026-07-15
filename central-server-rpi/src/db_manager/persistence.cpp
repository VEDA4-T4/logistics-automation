#include "logistics/central_server/persistence.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {
namespace {

constexpr std::int64_t kMillisecondsPerDay = 86'400'000;

std::uint32_t RotateRight(std::uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32U - shift));
}

std::string Sha256(std::span<const std::uint8_t> bytes) {
    static constexpr std::array<std::uint32_t, 64> constants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };
    std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) {
        data.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }
    std::array<std::uint32_t, 8> hash = { 0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                          0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U };
    for (std::size_t offset = 0; offset < data.size(); offset += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t at = offset + index * 4U;
            words[index] = (static_cast<std::uint32_t>(data[at]) << 24U) |
                           (static_cast<std::uint32_t>(data[at + 1]) << 16U) |
                           (static_cast<std::uint32_t>(data[at + 2]) << 8U) | data[at + 3];
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = RotateRight(words[index - 15], 7) ^ RotateRight(words[index - 15], 18) ^
                            (words[index - 15] >> 3U);
            const auto s1 = RotateRight(words[index - 2], 17) ^ RotateRight(words[index - 2], 19) ^
                            (words[index - 2] >> 10U);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = hash;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + s1 + choice + constants[index] + words[index];
            const auto s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : hash) {
        output << std::setw(8) << value;
    }
    return output.str();
}

std::string GenerateUuid() {
    std::array<std::uint8_t, 16> value{};
    std::random_device random;
    for (auto& byte : value) {
        byte = static_cast<std::uint8_t>(random());
    }
    value[6] = static_cast<std::uint8_t>((value[6] & 0x0fU) | 0x40U);
    value[8] = static_cast<std::uint8_t>((value[8] & 0x3fU) | 0x80U);
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            result << '-';
        }
        result << std::setw(2) << static_cast<int>(value[index]);
    }
    return result.str();
}

DatabaseStatus BindOptional(Statement& statement, int index, const std::optional<std::string>& value) {
    return value ? statement.Bind(index, *value) : statement.BindNull(index);
}

PersistenceResult Failure(const DatabaseStatus& status) {
    return { status.retryable() ? PersistenceStatus::kRetryableError : PersistenceStatus::kPermanentError,
             status.message, std::nullopt };
}

bool RequiresWorkId(contracts::mqtt::MessageType type) {
    using contracts::mqtt::MessageType;
    return type == MessageType::kPositionDetected || type == MessageType::kBarcodeDetected ||
           type == MessageType::kProductImage || type == MessageType::kProductInfo ||
           type == MessageType::kDestinationSet || type == MessageType::kWorkCompleted;
}

bool IsUuid(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f') ||
                     (value[index] >= 'A' && value[index] <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool IsDeviceStatus(contracts::mqtt::MessageType type) {
    using contracts::mqtt::MessageType;
    return type == MessageType::kDeviceRegister || type == MessageType::kHeartbeat ||
           type == MessageType::kDeviceStatus;
}

std::string ExtensionFor(std::string_view mime_type) {
    if (mime_type == "image/jpeg") return ".jpg";
    if (mime_type == "image/png") return ".png";
    if (mime_type == "image/webp") return ".webp";
    return {};
}

std::tm UtcTime(std::int64_t milliseconds) {
    const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm result{};
#ifdef _WIN32
    gmtime_s(&result, &seconds);
#else
    gmtime_r(&seconds, &result);
#endif
    return result;
}

}  // namespace

std::int64_t CurrentUnixTimeMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ImageStore::ImageStore(std::filesystem::path root) : root_(std::move(root)) {}

DatabaseStatus ImageStore::Store(std::string_view work_id, std::string_view mime_type,
                                 std::span<const std::uint8_t> bytes, std::int64_t captured_at_ms,
                                 StoredImage& output) const {
    const std::string extension = ExtensionFor(mime_type);
    if (!IsUuid(work_id) || extension.empty() || bytes.empty() || captured_at_ms < 0) {
        return { DatabaseStatusCode::kInvalidArgument, "invalid image metadata or empty image" };
    }
    const std::string digest = Sha256(bytes);
    const std::tm time = UtcTime(captured_at_ms);
    std::ostringstream year;
    std::ostringstream month;
    year << std::setfill('0') << std::setw(4) << time.tm_year + 1900;
    month << std::setfill('0') << std::setw(2) << time.tm_mon + 1;
    const auto relative = std::filesystem::path(year.str()) / month.str() / std::string(work_id) /
                          (digest + extension);
    const auto destination = root_ / relative;
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return { DatabaseStatusCode::kIoError, "cannot create image directory: " + error.message() };
    }
    if (std::filesystem::exists(destination, error) && !error) {
        output = { relative, digest, static_cast<std::int64_t>(bytes.size()), false };
        return DatabaseStatus::Ok();
    }
    const auto temporary = destination.string() + ".tmp-" + GenerateUuid();
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, error);
            return { DatabaseStatusCode::kIoError, "cannot write image temporary file" };
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return { DatabaseStatusCode::kIoError, "cannot publish image file: " + error.message() };
    }
    output = { relative, digest, static_cast<std::int64_t>(bytes.size()), true };
    return DatabaseStatus::Ok();
}

DatabaseStatus ImageStore::Remove(const std::filesystem::path& relative_path) const {
    if (relative_path.empty() || relative_path.is_absolute()) {
        return { DatabaseStatusCode::kInvalidArgument, "image path must be relative" };
    }
    std::error_code error;
    std::filesystem::remove(root_ / relative_path, error);
    return error ? DatabaseStatus{ DatabaseStatusCode::kIoError, "cannot remove image: " + error.message() }
                 : DatabaseStatus::Ok();
}

DatabaseStatus EventRepository::RecordReceived(const contracts::mqtt::EnvelopeView& envelope,
                                               const TransportMetadata& metadata, bool& duplicate) {
    Statement lookup;
    auto status = database_.Prepare("SELECT id FROM mqtt_event_log WHERE message_id=?", lookup);
    if (!status.ok() || !(status = lookup.Bind(1, envelope.message_id)).ok()) return status;
    bool row = false;
    if (!(status = lookup.Step(row)).ok()) return status;
    duplicate = row;
    if (duplicate) {
        Statement update;
        status = database_.Prepare(
            "UPDATE mqtt_event_log SET duplicate_count=duplicate_count+1,last_received_at_ms=? WHERE message_id=?",
            update);
        if (!status.ok() || !(status = update.Bind(1, metadata.received_at_ms)).ok() ||
            !(status = update.Bind(2, envelope.message_id)).ok()) return status;
        return update.Step(row);
    }
    Statement insert;
    status = database_.Prepare(
        "INSERT INTO mqtt_event_log(message_id,topic,protocol_version,message_type,source_id,payload_json,qos,"
        "retained,processing_state,received_at_ms,last_received_at_ms) VALUES(?,?,?,?,?,?,?,?, 'RECEIVED',?,?)",
        insert);
    if (!status.ok() || !(status = insert.Bind(1, envelope.message_id)).ok() ||
        !(status = insert.Bind(2, metadata.topic)).ok() || !(status = insert.Bind(3, envelope.protocol_version)).ok() ||
        !(status = insert.Bind(4, contracts::mqtt::ToString(envelope.message_type))).ok() ||
        !(status = insert.Bind(5, envelope.source_id)).ok() || !(status = insert.Bind(6, metadata.raw_payload)).ok() ||
        !(status = insert.Bind(7, metadata.qos)).ok() || !(status = insert.Bind(8, metadata.retained ? 1 : 0)).ok() ||
        !(status = insert.Bind(9, metadata.received_at_ms)).ok() ||
        !(status = insert.Bind(10, metadata.received_at_ms)).ok()) return status;
    return insert.Step(row);
}

DatabaseStatus EventRepository::MarkStored(std::string_view message_id, std::int64_t processed_at_ms) {
    Statement statement;
    auto status = database_.Prepare(
        "UPDATE mqtt_event_log SET processing_state='STORED',processed_at_ms=? WHERE message_id=?", statement);
    if (!status.ok() || !(status = statement.Bind(1, processed_at_ms)).ok() ||
        !(status = statement.Bind(2, message_id)).ok()) return status;
    bool row = false;
    return statement.Step(row);
}

DatabaseStatus EventRepository::MarkRejected(std::string_view message_id, std::string_view reason,
                                             std::int64_t processed_at_ms) {
    Statement statement;
    auto status = database_.Prepare(
        "UPDATE mqtt_event_log SET processing_state='REJECTED',failure_reason=?,processed_at_ms=? WHERE message_id=?",
        statement);
    if (!status.ok() || !(status = statement.Bind(1, reason)).ok() ||
        !(status = statement.Bind(2, processed_at_ms)).ok() || !(status = statement.Bind(3, message_id)).ok()) return status;
    bool row = false;
    return statement.Step(row);
}

DatabaseStatus ProductRepository::Create(std::string_view work_id, std::int64_t now_ms) {
    Statement statement;
    auto status = database_.Prepare(
        "INSERT INTO product(work_id,lifecycle_state,created_at_ms,updated_at_ms) VALUES(?,'DETECTED',?,?)",
        statement);
    if (!status.ok() || !(status = statement.Bind(1, work_id)).ok() || !(status = statement.Bind(2, now_ms)).ok() ||
        !(status = statement.Bind(3, now_ms)).ok()) return status;
    bool row = false;
    return statement.Step(row);
}

DatabaseStatus ProductRepository::ApplyEvent(std::string_view work_id, contracts::mqtt::MessageType type,
                                             const EventPayload& payload, std::int64_t now_ms) {
    std::string sql;
    using contracts::mqtt::MessageType;
    if (type == MessageType::kBarcodeDetected) {
        if (!payload.barcode || payload.barcode->empty())
            return { DatabaseStatusCode::kInvalidArgument, "BARCODE_DETECTED requires barcode" };
        sql = "UPDATE product SET barcode=?,lifecycle_state='IDENTIFIED',updated_at_ms=? WHERE work_id=?";
    } else if (type == MessageType::kProductInfo) {
        if (!payload.product_name || payload.product_name->empty())
            return { DatabaseStatusCode::kInvalidArgument, "PRODUCT_INFO requires product_name" };
        sql = "UPDATE product SET product_name=?,lifecycle_state='IDENTIFIED',updated_at_ms=? WHERE work_id=?";
    } else if (type == MessageType::kDestinationSet) {
        if (!payload.destination || payload.destination->empty())
            return { DatabaseStatusCode::kInvalidArgument, "DESTINATION_SET requires destination" };
        sql = "UPDATE product SET destination=?,lifecycle_state='DESTINATION_SET',updated_at_ms=? WHERE work_id=?";
    } else if (type == MessageType::kProductImage) {
        sql = "UPDATE product SET lifecycle_state='IMAGED',updated_at_ms=? WHERE work_id=?";
    } else if (type == MessageType::kWorkCompleted) {
        sql = "UPDATE product SET lifecycle_state='COMPLETED',updated_at_ms=?,completed_at_ms=? WHERE work_id=?";
    } else {
        sql = "UPDATE product SET updated_at_ms=? WHERE work_id=?";
    }
    Statement statement;
    auto status = database_.Prepare(sql, statement);
    if (!status.ok()) return status;
    int index = 1;
    if (type == MessageType::kBarcodeDetected) status = statement.Bind(index++, *payload.barcode);
    if (type == MessageType::kProductInfo) status = statement.Bind(index++, *payload.product_name);
    if (type == MessageType::kDestinationSet) status = statement.Bind(index++, *payload.destination);
    if (!status.ok() || !(status = statement.Bind(index++, now_ms)).ok()) return status;
    if (type == MessageType::kWorkCompleted && !(status = statement.Bind(index++, now_ms)).ok()) return status;
    if (
        !(status = statement.Bind(index, work_id)).ok()) return status;
    bool row = false;
    if (!(status = statement.Step(row)).ok()) return status;
    Statement changes;
    status = database_.Prepare("SELECT changes()", changes);
    if (!status.ok() || !(status = changes.Step(row)).ok()) return status;
    return changes.ColumnInt(0) == 0 ? DatabaseStatus{ DatabaseStatusCode::kNotFound, "work_id not found" }
                                     : DatabaseStatus::Ok();
}

DatabaseStatus ProductRepository::AppendHistory(std::string_view work_id, std::string_view message_id,
                                                contracts::mqtt::MessageType type, std::string_view source_id,
                                                const EventPayload& payload, std::int64_t occurred_at_ms) {
    Statement statement;
    auto status = database_.Prepare(
        "INSERT INTO work_history(work_id,message_id,event_type,process_state,source_id,details_json,occurred_at_ms) "
        "VALUES(?,?,?,?,?,?,?)", statement);
    if (!status.ok() || !(status = statement.Bind(1, work_id)).ok() ||
        !(status = statement.Bind(2, message_id)).ok() ||
        !(status = statement.Bind(3, contracts::mqtt::ToString(type))).ok() ||
        !(status = BindOptional(statement, 4, payload.process_state)).ok() ||
        !(status = statement.Bind(5, source_id)).ok() || !(status = statement.Bind(6, payload.details_json)).ok() ||
        !(status = statement.Bind(7, occurred_at_ms)).ok()) return status;
    bool row = false;
    return statement.Step(row);
}

DatabaseStatus DeviceRepository::AppendStatus(std::string_view message_id, std::string_view device_id,
                                              const EventPayload& payload, std::int64_t observed_at_ms) {
    Statement statement;
    auto status = database_.Prepare(
        "INSERT INTO device_status(device_id,message_id,role,connection_state,process_state,status_json,observed_at_ms) "
        "VALUES(?,?,?,?,?,?,?)", statement);
    const std::string connection = payload.connection_state.value_or("ONLINE");
    if (!status.ok() || !(status = statement.Bind(1, device_id)).ok() ||
        !(status = statement.Bind(2, message_id)).ok() || !(status = BindOptional(statement, 3, payload.device_role)).ok() ||
        !(status = statement.Bind(4, connection)).ok() || !(status = BindOptional(statement, 5, payload.process_state)).ok() ||
        !(status = statement.Bind(6, payload.details_json)).ok() ||
        !(status = statement.Bind(7, observed_at_ms)).ok()) return status;
    bool row = false;
    return statement.Step(row);
}

DatabaseStatus LogRepository::AppendError(std::string_view message_id, std::string_view source_id,
                                          const EventPayload& payload, std::int64_t occurred_at_ms) {
    if (!payload.error_code || !payload.severity || !payload.error_message) {
        return { DatabaseStatusCode::kInvalidArgument, "ERROR_OCCURRED requires code, severity and message" };
    }
    Statement statement;
    auto status = database_.Prepare(
        "INSERT INTO error_log(message_id,work_id,device_id,component_id,error_code,severity,error_message,"
        "details_json,occurred_at_ms) VALUES(?,?,?,?,?,?,?,?,?)", statement);
    if (!status.ok() || !(status = statement.Bind(1, message_id)).ok() ||
        !(status = BindOptional(statement, 2, payload.work_id)).ok() || !(status = statement.Bind(3, source_id)).ok() ||
        !(status = BindOptional(statement, 4, payload.component_id)).ok() ||
        !(status = statement.Bind(5, *payload.error_code)).ok() || !(status = statement.Bind(6, *payload.severity)).ok() ||
        !(status = statement.Bind(7, *payload.error_message)).ok() ||
        !(status = statement.Bind(8, payload.details_json)).ok() ||
        !(status = statement.Bind(9, occurred_at_ms)).ok()) return status;
    bool row = false;
    return statement.Step(row);
}

DatabaseStatus LogRepository::AppendSecurity(std::string_view event_type, std::string_view actor_id,
                                             std::string_view source_address, std::string_view details_json,
                                             std::int64_t occurred_at_ms) {
    Statement statement;
    auto status = database_.Prepare(
        "INSERT INTO security_log(event_type,actor_id,source_address,outcome,details_json,occurred_at_ms) "
        "VALUES(?,?,?,'INVALID',?,?)", statement);
    if (!status.ok() || !(status = statement.Bind(1, event_type)).ok() ||
        !(status = statement.Bind(2, actor_id)).ok() || !(status = statement.Bind(3, source_address)).ok() ||
        !(status = statement.Bind(4, details_json)).ok() || !(status = statement.Bind(5, occurred_at_ms)).ok()) return status;
    bool row = false;
    return statement.Step(row);
}

PersistenceService::PersistenceService(Database& database, StorageConfig storage_config)
    : database_(database),
      storage_config_(std::move(storage_config)),
      image_store_(storage_config_.image_root),
      next_cleanup_at_ms_(CurrentUnixTimeMilliseconds() +
                          std::max(1, storage_config_.cleanup_interval_hours) * 3'600'000LL) {}

DatabaseStatus PersistenceService::RunRetentionIfDue(std::int64_t now_ms) {
    if (now_ms < next_cleanup_at_ms_) return DatabaseStatus::Ok();
    RetentionService retention(database_, storage_config_);
    auto status = retention.RunOnce(now_ms);
    if (status.ok()) {
        next_cleanup_at_ms_ = now_ms + std::max(1, storage_config_.cleanup_interval_hours) * 3'600'000LL;
    }
    return status;
}

PersistenceResult PersistenceService::PersistValidatedEvent(const contracts::mqtt::EnvelopeView& envelope,
                                                             const EventPayload& payload,
                                                             const TransportMetadata& metadata) {
    const auto cleanup_status = RunRetentionIfDue(CurrentUnixTimeMilliseconds());
    if (!cleanup_status.ok()) return Failure(cleanup_status);
    const auto parsed_topic = contracts::mqtt::ParseTopic(metadata.topic);
    if (!envelope.IsValid() || !parsed_topic.IsValid() || metadata.received_at_ms < 0 ||
        (parsed_topic.endpoint_id.size() > 0 && parsed_topic.endpoint_id != envelope.source_id)) {
        LogRepository logs(database_);
        const auto security = logs.AppendSecurity("INVALID_MQTT_ENVELOPE", envelope.source_id, metadata.source_address,
                                                  "{\"reason\":\"envelope or topic/source validation failed\"}",
                                                  metadata.received_at_ms);
        if (!security.ok()) return Failure(security);
        return { PersistenceStatus::kPermanentError, "envelope or topic/source validation failed", std::nullopt };
    }
    if (metadata.qos != 0 && metadata.qos != 1) {
        return { PersistenceStatus::kPermanentError, "invalid QoS", std::nullopt };
    }
    Transaction transaction(database_);
    if (!transaction.status().ok()) return Failure(transaction.status());
    EventRepository events(database_);
    bool duplicate = false;
    auto status = events.RecordReceived(envelope, metadata, duplicate);
    if (!status.ok()) return Failure(status);
    if (duplicate) {
        status = transaction.Commit();
        if (!status.ok()) return Failure(status);
        return { PersistenceStatus::kDuplicate, "message was already stored", payload.work_id };
    }

    ProductRepository products(database_);
    std::optional<std::string> work_id = payload.work_id;
    StoredImage stored_image;
    bool remove_image_on_failure = false;
    status = database_.Execute("SAVEPOINT derived_event");
    if (!status.ok()) return Failure(status);
    auto reject = [&](const DatabaseStatus& cause) -> PersistenceResult {
        if (remove_image_on_failure) static_cast<void>(image_store_.Remove(stored_image.relative_path));
        if (cause.retryable()) return Failure(cause);
        auto cleanup = database_.Execute("ROLLBACK TO derived_event; RELEASE derived_event");
        if (!cleanup.ok()) return Failure(cleanup);
        cleanup = events.MarkRejected(envelope.message_id, cause.message, CurrentUnixTimeMilliseconds());
        if (!cleanup.ok()) return Failure(cleanup);
        cleanup = transaction.Commit();
        return cleanup.ok() ? Failure(cause) : Failure(cleanup);
    };
    if (RequiresWorkId(envelope.message_type) && (!work_id || !IsUuid(*work_id))) {
        return reject({ DatabaseStatusCode::kInvalidArgument, "message requires a UUID work_id" });
    }
    if (envelope.message_type == contracts::mqtt::MessageType::kBoxDetected) {
        work_id = GenerateUuid();
        if (!(status = products.Create(*work_id, metadata.received_at_ms)).ok() ||
            !(status = products.AppendHistory(*work_id, envelope.message_id, envelope.message_type, envelope.source_id,
                                              payload, metadata.received_at_ms)).ok()) return reject(status);
    } else if (RequiresWorkId(envelope.message_type)) {
        if (envelope.message_type == contracts::mqtt::MessageType::kProductImage) {
            if (!payload.image_mime_type || payload.image_bytes.empty())
                return reject({ DatabaseStatusCode::kInvalidArgument, "PRODUCT_IMAGE requires MIME type and bytes" });
            status = image_store_.Store(*work_id, *payload.image_mime_type, payload.image_bytes,
                                        payload.captured_at_ms.value_or(metadata.received_at_ms), stored_image);
            if (!status.ok()) return reject(status);
            remove_image_on_failure = stored_image.created;
            Statement image;
            status = database_.Prepare(
                "INSERT INTO image_file(work_id,message_id,relative_path,mime_type,byte_size,sha256,captured_at_ms,created_at_ms) "
                "VALUES(?,?,?,?,?,?,?,?)", image);
            if (status.ok()) status = image.Bind(1, *work_id);
            if (status.ok()) status = image.Bind(2, envelope.message_id);
            if (status.ok()) status = image.Bind(3, stored_image.relative_path.generic_string());
            if (status.ok()) status = image.Bind(4, *payload.image_mime_type);
            if (status.ok()) status = image.Bind(5, stored_image.byte_size);
            if (status.ok()) status = image.Bind(6, stored_image.sha256);
            if (status.ok()) status = image.Bind(7, payload.captured_at_ms.value_or(metadata.received_at_ms));
            if (status.ok()) status = image.Bind(8, metadata.received_at_ms);
            bool row = false;
            if (status.ok()) status = image.Step(row);
            if (!status.ok()) {
                return reject(status);
            }
        }
        if (!(status = products.ApplyEvent(*work_id, envelope.message_type, payload, metadata.received_at_ms)).ok() ||
            !(status = products.AppendHistory(*work_id, envelope.message_id, envelope.message_type, envelope.source_id,
                                              payload, metadata.received_at_ms)).ok()) {
            return reject(status);
        }
    } else if (IsDeviceStatus(envelope.message_type)) {
        DeviceRepository devices(database_);
        status = devices.AppendStatus(envelope.message_id, envelope.source_id, payload, metadata.received_at_ms);
        if (!status.ok()) return reject(status);
    } else if (envelope.message_type == contracts::mqtt::MessageType::kErrorOccurred) {
        LogRepository logs(database_);
        status = logs.AppendError(envelope.message_id, envelope.source_id, payload, metadata.received_at_ms);
        if (!status.ok()) return reject(status);
    }
    if (!(status = database_.Execute("RELEASE derived_event")).ok()) return Failure(status);
    if (!(status = events.MarkStored(envelope.message_id, CurrentUnixTimeMilliseconds())).ok() ||
        !(status = transaction.Commit()).ok()) {
        if (remove_image_on_failure) static_cast<void>(image_store_.Remove(stored_image.relative_path));
        return Failure(status);
    }
    return { PersistenceStatus::kStored, "message stored", work_id };
}

RetentionService::RetentionService(Database& database, StorageConfig config)
    : database_(database), config_(std::move(config)), image_store_(config_.image_root) {}

DatabaseStatus RetentionService::RunOnce(std::int64_t now_ms) {
    if (now_ms < 0) return { DatabaseStatusCode::kInvalidArgument, "invalid cleanup time" };
    const auto image_cutoff = now_ms - static_cast<std::int64_t>(config_.image_retention_days) * kMillisecondsPerDay;
    DatabaseStatus status;
    bool row = false;
    while (true) {
        Statement images;
        status = database_.Prepare("SELECT id,relative_path FROM image_file WHERE created_at_ms<? ORDER BY id LIMIT 500", images);
        if (!status.ok() || !(status = images.Bind(1, image_cutoff)).ok()) return status;
        std::vector<std::pair<std::int64_t, std::string>> expired_images;
        while ((status = images.Step(row)).ok() && row) {
            expired_images.emplace_back(images.ColumnInt64(0), images.ColumnText(1));
        }
        if (!status.ok()) return status;
        if (expired_images.empty()) break;
        std::size_t removed = 0;
        for (const auto& [id, path] : expired_images) {
            status = image_store_.Remove(path);
            if (!status.ok()) continue;
            Statement erase;
            status = database_.Prepare("DELETE FROM image_file WHERE id=?", erase);
            if (!status.ok() || !(status = erase.Bind(1, id)).ok() || !(status = erase.Step(row)).ok()) return status;
            ++removed;
        }
        // Every selected file failed deletion; retain metadata and retry on the next scheduled run.
        if (removed == 0) break;
    }
    struct RetentionSql { const char* sql; int days; };
    const std::array policies = {
        RetentionSql{ "DELETE FROM mqtt_event_log WHERE id IN (SELECT id FROM mqtt_event_log WHERE received_at_ms<? LIMIT 500)", config_.mqtt_retention_days },
        RetentionSql{ "DELETE FROM device_status WHERE id IN (SELECT d.id FROM device_status d WHERE d.observed_at_ms<? AND EXISTS (SELECT 1 FROM device_status newer WHERE newer.device_id=d.device_id AND newer.observed_at_ms>d.observed_at_ms) LIMIT 500)", config_.device_status_retention_days },
        RetentionSql{ "DELETE FROM error_log WHERE id IN (SELECT id FROM error_log WHERE occurred_at_ms<? LIMIT 500)", config_.error_retention_days },
        RetentionSql{ "DELETE FROM security_log WHERE id IN (SELECT id FROM security_log WHERE occurred_at_ms<? LIMIT 500)", config_.security_retention_days },
    };
    for (const auto& policy : policies) {
        const auto cutoff = now_ms - static_cast<std::int64_t>(policy.days) * kMillisecondsPerDay;
        while (true) {
            Transaction transaction(database_);
            if (!transaction.status().ok()) return transaction.status();
            Statement erase;
            status = database_.Prepare(policy.sql, erase);
            if (!status.ok() || !(status = erase.Bind(1, cutoff)).ok() || !(status = erase.Step(row)).ok()) return status;
            Statement changes;
            status = database_.Prepare("SELECT changes()", changes);
            if (!status.ok() || !(status = changes.Step(row)).ok()) return status;
            const int removed = changes.ColumnInt(0);
            if (!(status = transaction.Commit()).ok()) return status;
            if (removed == 0) break;
        }
    }
    return DatabaseStatus::Ok();
}

}  // namespace logistics::central_server
