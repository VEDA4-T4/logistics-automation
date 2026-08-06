#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace logistics::device {

struct LogFileMetadata {
    std::string device_id;
    std::string message_id;
    std::string started_at;
    std::string ended_at;
    std::string sha256;
    std::size_t byte_size{};
    int attempts{};
};

enum class UploadDisposition { kConfirmed, kRetryableFailure, kPermanentFailure };

struct LogUploadResult {
    UploadDisposition disposition{ UploadDisposition::kRetryableFailure };
    std::string checksum;
    std::string message;
};

class LogUploadTransport {
public:
    virtual ~LogUploadTransport() = default;
    [[nodiscard]] virtual LogUploadResult Upload(const std::filesystem::path& file,
                                                 const LogFileMetadata& metadata) = 0;
};

struct LogSpoolConfig {
    std::string device_id;
    std::string endpoint_url;
    std::string bearer_token;
    std::filesystem::path ca_certificate;
    std::filesystem::path spool_directory{ "/var/lib/logistics/log-spool" };
    std::size_t rotate_bytes{ 5U * 1024U * 1024U };
    std::chrono::seconds rotate_interval{ std::chrono::hours(1) };
    std::size_t maximum_spool_bytes{ 100U * 1024U * 1024U };
    std::chrono::seconds request_timeout{ 30 };
    int maximum_attempts{ 5 };
    std::chrono::seconds initial_backoff{ 1 };
    std::chrono::seconds maximum_backoff{ 60 };
    bool allow_insecure_http{ false };
};

[[nodiscard]] std::unique_ptr<LogUploadTransport> CreateCurlLogUploadTransport(const LogSpoolConfig& config);

class LogSpoolUploader final {
public:
    explicit LogSpoolUploader(LogSpoolConfig config, std::unique_ptr<LogUploadTransport> transport = nullptr);
    ~LogSpoolUploader();
    LogSpoolUploader(const LogSpoolUploader&) = delete;
    LogSpoolUploader& operator=(const LogSpoolUploader&) = delete;

    [[nodiscard]] bool Start();
    void Stop();
    [[nodiscard]] bool Append(std::string_view line);
    [[nodiscard]] bool RotateNow();
    [[nodiscard]] bool ProcessPendingOnce();
    [[nodiscard]] std::size_t PendingCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace logistics::device
