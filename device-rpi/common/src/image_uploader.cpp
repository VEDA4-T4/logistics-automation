#include "logistics/device/image_uploader.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

#include "logistics/contracts/http_upload.hpp"

namespace logistics::device {
namespace {

namespace contract = contracts::http;

std::string Sha256(std::span<const std::uint8_t> bytes) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return {};
    }
    bool succeeded = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                     EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    succeeded = succeeded && EVP_DigestFinal_ex(context, digest.data(), &digest_size) == 1;
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

}  // namespace

ImageUploader::ImageUploader(ImageUploadConfig config, std::unique_ptr<ImageUploadTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    if (transport_ == nullptr) {
        transport_ = CreateCurlImageUploadTransport(config_);
    }
}

ImageUploadResult ImageUploader::Upload(std::string device_id, std::string work_id, std::string message_id,
                                        std::string captured_at, std::string image_name, std::string mime_type,
                                        std::span<const std::uint8_t> bytes) const {
    if (!config_.IsValid() || transport_ == nullptr || bytes.empty()) {
        return { ImageUploadDisposition::kPermanentFailure, {}, {}, {}, "invalid image upload configuration" };
    }
    ImageUploadRequest request{
        .device_id = std::move(device_id),
        .work_id = std::move(work_id),
        .message_id = std::move(message_id),
        .captured_at = std::move(captured_at),
        .image_name = std::move(image_name),
        .mime_type = std::move(mime_type),
        .sha256 = Sha256(bytes),
        .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
    };
    const auto validation = contract::Validate({
        .kind = contract::UploadKind::kImage,
        .device_id = request.device_id,
        .work_id = request.work_id,
        .message_id = request.message_id,
        .captured_at = request.captured_at,
        .started_at = {},
        .ended_at = {},
        .sha256 = request.sha256,
        .mime_type = request.mime_type,
        .byte_size = request.bytes.size(),
    });
    if (validation != contract::ValidationError::kNone) {
        return { ImageUploadDisposition::kPermanentFailure, {}, {}, {}, "invalid image upload metadata" };
    }

    auto backoff = config_.initial_backoff;
    ImageUploadResult result;
    for (int attempt = 1; attempt <= config_.maximum_attempts; ++attempt) {
        result = transport_->Upload(request);
        if (result.disposition != ImageUploadDisposition::kRetryableFailure || attempt == config_.maximum_attempts) {
            break;
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, config_.maximum_backoff);
    }
    if (result.IsConfirmed() && (!contracts::IsValidUuid(result.upload_id) || result.checksum != request.sha256 ||
                                 !result.path.starts_with("/uploads/images/"))) {
        return { ImageUploadDisposition::kPermanentFailure, result.upload_id, result.path, result.checksum,
                 "server image metadata does not match the upload" };
    }
    return result;
}

}  // namespace logistics::device
