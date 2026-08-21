#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace logistics::device {

struct ImageUploadConfig final {
    std::string endpoint_url;
    std::string bearer_token;
    std::filesystem::path ca_certificate;
    std::chrono::seconds request_timeout{ 30 };
    int maximum_attempts{ 5 };
    std::chrono::seconds initial_backoff{ 1 };
    std::chrono::seconds maximum_backoff{ 60 };
    bool allow_insecure_http{ false };

    [[nodiscard]] bool IsValid() const noexcept {
        const bool endpoint_allowed =
            endpoint_url.starts_with("https://") || (allow_insecure_http && endpoint_url.starts_with("http://"));
        return endpoint_allowed && !bearer_token.empty() && request_timeout.count() > 0 && maximum_attempts > 0 &&
               initial_backoff.count() > 0 && maximum_backoff >= initial_backoff;
    }
};

struct ImageUploadRequest final {
    std::string device_id;
    std::string work_id;
    std::string message_id;
    std::string captured_at;
    std::string image_name;
    std::string mime_type;
    std::string sha256;
    std::vector<std::uint8_t> bytes;
};

enum class ImageUploadDisposition { kConfirmed, kRetryableFailure, kPermanentFailure };

struct ImageUploadResult final {
    ImageUploadDisposition disposition{ ImageUploadDisposition::kRetryableFailure };
    std::string upload_id;
    std::string path;
    std::string checksum;
    std::string error;

    [[nodiscard]] bool IsConfirmed() const noexcept {
        return disposition == ImageUploadDisposition::kConfirmed;
    }
};

class ImageUploadTransport {
public:
    virtual ~ImageUploadTransport() = default;
    [[nodiscard]] virtual ImageUploadResult Upload(const ImageUploadRequest& request) = 0;
};

[[nodiscard]] std::unique_ptr<ImageUploadTransport> CreateCurlImageUploadTransport(const ImageUploadConfig& config);

class ImageUploader final {
public:
    explicit ImageUploader(ImageUploadConfig config, std::unique_ptr<ImageUploadTransport> transport = nullptr);

    [[nodiscard]] ImageUploadResult Upload(std::string device_id, std::string work_id, std::string message_id,
                                           std::string captured_at, std::string image_name, std::string mime_type,
                                           std::span<const std::uint8_t> bytes) const;

private:
    ImageUploadConfig config_;
    std::unique_ptr<ImageUploadTransport> transport_;
};

}  // namespace logistics::device
