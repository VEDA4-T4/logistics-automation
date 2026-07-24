#include "logistics/device/log_spool_uploader.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "logistics/contracts/http_upload.hpp"

namespace logistics::device {
namespace {

namespace contract = contracts::http;

constexpr std::string_view kActiveLogName = "active.log";
constexpr std::string_view kActiveMetadataName = "active.meta";
constexpr std::string_view kPendingSuffix = ".pending.log";

std::string UtcNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string GenerateMessageId() {
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

std::string Sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!input || context == nullptr) {
        EVP_MD_CTX_free(context);
        return {};
    }
    bool succeeded = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    std::array<char, 16U * 1024U> buffer{};
    while (succeeded && input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            succeeded = EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) == 1;
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    succeeded = succeeded && input.eof() && EVP_DigestFinal_ex(context, digest.data(), &digest_size) == 1;
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

bool WriteMetadata(const std::filesystem::path& path, const LogFileMetadata& metadata) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << "deviceId=" << metadata.device_id << '\n'
               << "messageId=" << metadata.message_id << '\n'
               << "startedAt=" << metadata.started_at << '\n'
               << "endedAt=" << metadata.ended_at << '\n'
               << "sha256=" << metadata.sha256 << '\n'
               << "byteSize=" << metadata.byte_size << '\n'
               << "attempts=" << metadata.attempts << '\n';
        if (!output) {
            return false;
        }
    }
    std::error_code error;
#ifdef _WIN32
    std::filesystem::remove(path, error);
    error.clear();
#endif
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool ReadMetadata(const std::filesystem::path& path, LogFileMetadata& metadata) {
    std::ifstream input(path);
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos) {
            fields[line.substr(0, separator)] = line.substr(separator + 1);
        }
    }
    if (!input.eof()) {
        return false;
    }
    try {
        metadata.device_id = fields.at("deviceId");
        metadata.message_id = fields.at("messageId");
        metadata.started_at = fields.at("startedAt");
        metadata.ended_at = fields.at("endedAt");
        metadata.sha256 = fields.at("sha256");
        metadata.byte_size = static_cast<std::size_t>(std::stoull(fields.at("byteSize")));
        metadata.attempts = std::stoi(fields.at("attempts"));
    } catch (...) {
        return false;
    }
    return contract::Validate({ .kind = contract::UploadKind::kLog,
                                .device_id = metadata.device_id,
                                .work_id = {},
                                .message_id = metadata.message_id,
                                .captured_at = {},
                                .started_at = metadata.started_at,
                                .ended_at = metadata.ended_at,
                                .sha256 = metadata.sha256,
                                .mime_type = "text/plain",
                                .byte_size = metadata.byte_size }) == contract::ValidationError::kNone;
}

std::filesystem::path MetadataPath(const std::filesystem::path& log_path) {
    auto metadata_path = log_path;
    metadata_path.replace_extension(".meta");
    return metadata_path;
}

std::vector<std::filesystem::path> PendingLogs(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.ends_with(kPendingSuffix)) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        std::error_code left_error;
        std::error_code right_error;
        return std::filesystem::last_write_time(left, left_error) <
               std::filesystem::last_write_time(right, right_error);
    });
    return files;
}

std::size_t CurlWrite(char* data, std::size_t size, std::size_t count, void* output_pointer) {
    auto& output = *static_cast<std::string*>(output_pointer);
    output.append(data, size * count);
    return size * count;
}

std::string JsonChecksum(std::string_view response) {
    constexpr std::string_view prefix = "\"checksum\":\"";
    const auto start = response.find(prefix);
    if (start == std::string_view::npos) {
        return {};
    }
    const auto value_start = start + prefix.size();
    const auto end = response.find('"', value_start);
    return end == std::string_view::npos ? std::string{} : std::string(response.substr(value_start, end - value_start));
}

class CurlLogUploadTransport final : public LogUploadTransport {
public:
    explicit CurlLogUploadTransport(LogSpoolConfig config) : config_(std::move(config)) {}

    LogUploadResult Upload(const std::filesystem::path& file, const LogFileMetadata& metadata) override {
        const bool secure = config_.endpoint_url.starts_with("https://");
        if (!secure && !(config_.allow_insecure_http && config_.endpoint_url.starts_with("http://"))) {
            return { UploadDisposition::kPermanentFailure, {}, "HTTPS is required for the upload endpoint" };
        }
        static std::once_flag curl_once;
        std::call_once(curl_once, [] { static_cast<void>(curl_global_init(CURL_GLOBAL_DEFAULT)); });
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            return { UploadDisposition::kRetryableFailure, {}, "cannot initialize libcurl" };
        }

        std::string response;
        const std::string byte_size = std::to_string(metadata.byte_size);
        const std::string authorization = "Authorization: Bearer " + config_.bearer_token;
        const std::string idempotency = "Idempotency-Key: " + metadata.message_id;
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, authorization.c_str());
        headers = curl_slist_append(headers, idempotency.c_str());
        curl_mime* mime = curl_mime_init(curl);
        AddField(mime, contract::kDeviceIdField, metadata.device_id);
        AddField(mime, contract::kMessageIdField, metadata.message_id);
        AddField(mime, contract::kStartedAtField, metadata.started_at);
        AddField(mime, contract::kEndedAtField, metadata.ended_at);
        AddField(mime, contract::kChecksumField, metadata.sha256);
        AddField(mime, contract::kByteSizeField, byte_size);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, contract::kFileField.data());
        curl_mime_type(part, "text/plain");
        curl_mime_filedata(part, file.string().c_str());

        curl_easy_setopt(curl, CURLOPT_URL, config_.endpoint_url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, config_.request_timeout.count());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.request_timeout.count());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        const std::string ca_certificate = config_.ca_certificate.string();
        if (!ca_certificate.empty()) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_certificate.c_str());
        }
        const CURLcode code = curl_easy_perform(curl);
        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (code != CURLE_OK) {
            return { UploadDisposition::kRetryableFailure, {}, curl_easy_strerror(code) };
        }
        if (http_status == 200 || http_status == 201) {
            const std::string checksum = JsonChecksum(response);
            if (checksum == metadata.sha256) {
                return { UploadDisposition::kConfirmed, checksum, "upload confirmed" };
            }
            return { UploadDisposition::kPermanentFailure, checksum, "server checksum is missing or different" };
        }
        if (http_status == 408 || http_status == 429 || http_status >= 500) {
            return { UploadDisposition::kRetryableFailure, {}, "transient HTTP status " + std::to_string(http_status) };
        }
        return { UploadDisposition::kPermanentFailure,
                 {},
                 "HTTP upload rejected with status " + std::to_string(http_status) };
    }

private:
    static void AddField(curl_mime* mime, std::string_view name, const std::string& value) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, name.data());
        curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    }

    LogSpoolConfig config_;
};

}  // namespace

std::unique_ptr<LogUploadTransport> CreateCurlLogUploadTransport(const LogSpoolConfig& config) {
    return std::make_unique<CurlLogUploadTransport>(config);
}

class LogSpoolUploader::Impl final {
public:
    Impl(LogSpoolConfig config, std::unique_ptr<LogUploadTransport> transport)
        : config_(std::move(config)), transport_(std::move(transport)) {
        if (transport_ == nullptr) {
            transport_ = CreateCurlLogUploadTransport(config_);
        }
    }

    ~Impl() {
        Stop();
    }

    bool Start() {
        std::lock_guard lock(mutex_);
        if (!InitializeLocked()) {
            return false;
        }
        if (!worker_.joinable()) {
            stop_requested_ = false;
            worker_ = std::thread([this] { WorkerLoop(); });
        }
        return true;
    }

    void Stop() {
        {
            std::lock_guard lock(mutex_);
            stop_requested_ = true;
        }
        wakeup_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool Append(std::string_view line) {
        std::lock_guard lock(mutex_);
        if (!InitializeLocked()) {
            return false;
        }
        std::ofstream output(ActiveLogPath(), std::ios::app);
        output << UtcNow() << ' ' << line << '\n';
        if (!output) {
            return false;
        }
        output.close();
        std::error_code error;
        const auto size = std::filesystem::file_size(ActiveLogPath(), error);
        const bool time_due = std::chrono::steady_clock::now() - active_started_ >= config_.rotate_interval;
        if (!error && (size >= config_.rotate_bytes || time_due) && !RotateLocked()) {
            return false;
        }
        wakeup_.notify_one();
        return true;
    }

    bool RotateNow() {
        std::lock_guard lock(mutex_);
        return InitializeLocked() && RotateLocked();
    }

    bool ProcessPendingOnce() {
        std::filesystem::path log_path;
        LogFileMetadata metadata;
        {
            std::lock_guard lock(mutex_);
            if (!InitializeLocked()) {
                return false;
            }
            for (const auto& candidate : PendingLogs(config_.spool_directory)) {
                LogFileMetadata candidate_metadata;
                if (!ReadMetadata(MetadataPath(candidate), candidate_metadata) ||
                    candidate_metadata.attempts >= config_.maximum_attempts) {
                    continue;
                }
                const auto retry = next_attempt_.find(candidate_metadata.message_id);
                if (retry != next_attempt_.end() && std::chrono::steady_clock::now() < retry->second) {
                    continue;
                }
                log_path = candidate;
                metadata = std::move(candidate_metadata);
                break;
            }
            if (log_path.empty()) {
                return false;
            }
        }

        const LogUploadResult result = transport_->Upload(log_path, metadata);
        std::lock_guard lock(mutex_);
        if (result.disposition == UploadDisposition::kConfirmed && result.checksum == metadata.sha256) {
            std::error_code error;
            std::filesystem::remove(log_path, error);
            std::filesystem::remove(MetadataPath(log_path), error);
            next_attempt_.erase(metadata.message_id);
            return true;
        }
        metadata.attempts = result.disposition == UploadDisposition::kPermanentFailure ? config_.maximum_attempts
                                                                                       : metadata.attempts + 1;
        if (!WriteMetadata(MetadataPath(log_path), metadata)) {
            return false;
        }
        if (result.disposition == UploadDisposition::kRetryableFailure &&
            metadata.attempts < config_.maximum_attempts) {
            const int exponent = std::min(metadata.attempts - 1, 20);
            const auto multiplier = std::uint64_t{ 1 } << static_cast<unsigned int>(exponent);
            const auto requested =
                std::chrono::seconds(config_.initial_backoff.count() * static_cast<std::int64_t>(multiplier));
            next_attempt_[metadata.message_id] =
                std::chrono::steady_clock::now() + std::min(requested, config_.maximum_backoff);
        }
        return true;
    }

    std::size_t PendingCount() const {
        std::lock_guard lock(mutex_);
        std::error_code error;
        if (!std::filesystem::is_directory(config_.spool_directory, error)) {
            return 0;
        }
        return PendingLogs(config_.spool_directory).size();
    }

private:
    bool InitializeLocked() {
        if (initialized_) {
            return true;
        }
        if (config_.device_id.empty() || config_.rotate_bytes == 0 || config_.maximum_spool_bytes == 0 ||
            config_.maximum_attempts <= 0) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(config_.spool_directory, error);
        if (error) {
            return false;
        }
        std::ifstream metadata(ActiveMetadataPath());
        std::getline(metadata, active_started_at_);
        if (active_started_at_.empty()) {
            active_started_at_ = UtcNow();
            std::ofstream output(ActiveMetadataPath(), std::ios::trunc);
            output << active_started_at_ << '\n';
            if (!output) {
                return false;
            }
        }
        active_started_ = std::chrono::steady_clock::now();
        initialized_ = true;
        EnforceSpoolLimitLocked();
        return true;
    }

    bool RotateLocked() {
        std::error_code error;
        if (!std::filesystem::exists(ActiveLogPath(), error) ||
            std::filesystem::file_size(ActiveLogPath(), error) == 0) {
            return !error;
        }
        const std::string message_id = GenerateMessageId();
        const auto pending = config_.spool_directory / (message_id + std::string(kPendingSuffix));
        std::filesystem::rename(ActiveLogPath(), pending, error);
        if (error) {
            return false;
        }
        LogFileMetadata metadata{ .device_id = config_.device_id,
                                  .message_id = message_id,
                                  .started_at = active_started_at_,
                                  .ended_at = UtcNow(),
                                  .sha256 = Sha256File(pending),
                                  .byte_size = std::filesystem::file_size(pending, error),
                                  .attempts = 0 };
        if (error || metadata.sha256.empty() || !WriteMetadata(MetadataPath(pending), metadata)) {
            return false;
        }
        active_started_at_ = UtcNow();
        active_started_ = std::chrono::steady_clock::now();
        std::ofstream active_metadata(ActiveMetadataPath(), std::ios::trunc);
        active_metadata << active_started_at_ << '\n';
        if (!active_metadata) {
            return false;
        }
        EnforceSpoolLimitLocked();
        wakeup_.notify_one();
        return true;
    }

    void EnforceSpoolLimitLocked() {
        auto files = PendingLogs(config_.spool_directory);
        std::uintmax_t total = 0;
        std::error_code error;
        for (const auto& file : files) {
            total += std::filesystem::file_size(file, error);
            error.clear();
        }
        for (const auto& file : files) {
            if (total <= config_.maximum_spool_bytes) {
                break;
            }
            const auto size = std::filesystem::file_size(file, error);
            if (error) {
                error.clear();
                continue;
            }
            std::filesystem::remove(file, error);
            std::filesystem::remove(MetadataPath(file), error);
            total -= size;
        }
    }

    void WorkerLoop() {
        while (true) {
            {
                std::unique_lock lock(mutex_);
                if (wakeup_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stop_requested_.load(); })) {
                    return;
                }
            }
            static_cast<void>(ProcessPendingOnce());
        }
    }

    [[nodiscard]] std::filesystem::path ActiveLogPath() const {
        return config_.spool_directory / kActiveLogName;
    }

    [[nodiscard]] std::filesystem::path ActiveMetadataPath() const {
        return config_.spool_directory / kActiveMetadataName;
    }

    LogSpoolConfig config_;
    std::unique_ptr<LogUploadTransport> transport_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::thread worker_;
    std::atomic_bool stop_requested_{ false };
    bool initialized_{ false };
    std::string active_started_at_;
    std::chrono::steady_clock::time_point active_started_;
    std::map<std::string, std::chrono::steady_clock::time_point> next_attempt_;
};

LogSpoolUploader::LogSpoolUploader(LogSpoolConfig config, std::unique_ptr<LogUploadTransport> transport)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(transport))) {}

LogSpoolUploader::~LogSpoolUploader() = default;

bool LogSpoolUploader::Start() {
    return impl_->Start();
}

void LogSpoolUploader::Stop() {
    impl_->Stop();
}

bool LogSpoolUploader::Append(std::string_view line) {
    return impl_->Append(line);
}

bool LogSpoolUploader::RotateNow() {
    return impl_->RotateNow();
}

bool LogSpoolUploader::ProcessPendingOnce() {
    return impl_->ProcessPendingOnce();
}

std::size_t LogSpoolUploader::PendingCount() const {
    return impl_->PendingCount();
}

}  // namespace logistics::device
