#include <curl/curl.h>

#include <mutex>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

#include "logistics/contracts/http_upload.hpp"
#include "logistics/device/image_uploader.hpp"

namespace logistics::device {
namespace {

namespace contract = contracts::http;

std::size_t CurlWrite(char* data, std::size_t size, std::size_t count, void* output_pointer) {
    auto& output = *static_cast<std::string*>(output_pointer);
    output.append(data, size * count);
    return size * count;
}

void AddField(curl_mime* mime, std::string_view name, const std::string& value) {
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, name.data());
    curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
}

class CurlImageUploadTransport final : public ImageUploadTransport {
public:
    explicit CurlImageUploadTransport(ImageUploadConfig config) : config_(std::move(config)) {}

    ImageUploadResult Upload(const ImageUploadRequest& request) override {
        static std::once_flag curl_once;
        std::call_once(curl_once, [] { static_cast<void>(curl_global_init(CURL_GLOBAL_DEFAULT)); });
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            return { ImageUploadDisposition::kRetryableFailure, {}, {}, {}, "cannot initialize libcurl" };
        }

        std::string response;
        const std::string authorization = "Authorization: Bearer " + config_.bearer_token;
        const std::string idempotency = "Idempotency-Key: " + request.message_id;
        const std::string byte_size = std::to_string(request.bytes.size());
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, authorization.c_str());
        headers = curl_slist_append(headers, idempotency.c_str());
        curl_mime* mime = curl_mime_init(curl);
        AddField(mime, contract::kDeviceIdField, request.device_id);
        AddField(mime, contract::kWorkIdField, request.work_id);
        AddField(mime, contract::kMessageIdField, request.message_id);
        AddField(mime, contract::kCapturedAtField, request.captured_at);
        AddField(mime, contract::kChecksumField, request.sha256);
        AddField(mime, contract::kByteSizeField, byte_size);
        curl_mimepart* file = curl_mime_addpart(mime);
        curl_mime_name(file, contract::kFileField.data());
        curl_mime_filename(file, request.image_name.c_str());
        curl_mime_type(file, request.mime_type.c_str());
        curl_mime_data(file, reinterpret_cast<const char*>(request.bytes.data()), request.bytes.size());

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
            return { ImageUploadDisposition::kRetryableFailure, {}, {}, {}, curl_easy_strerror(code) };
        }
        if (http_status == 200 || http_status == 201) {
            try {
                const auto json = nlohmann::json::parse(response);
                return {
                    .disposition = ImageUploadDisposition::kConfirmed,
                    .upload_id = json.at(std::string(contract::kUploadIdResponseField)).get<std::string>(),
                    .path = json.at(std::string(contract::kPathResponseField)).get<std::string>(),
                    .checksum = json.at(std::string(contract::kChecksumResponseField)).get<std::string>(),
                    .error = {},
                };
            } catch (const nlohmann::json::exception&) {
                return { ImageUploadDisposition::kPermanentFailure, {}, {}, {}, "invalid image upload response" };
            }
        }
        if (http_status == 408 || http_status == 429 || http_status >= 500) {
            return { ImageUploadDisposition::kRetryableFailure,
                     {},
                     {},
                     {},
                     "transient HTTP status " + std::to_string(http_status) };
        }
        return { ImageUploadDisposition::kPermanentFailure,
                 {},
                 {},
                 {},
                 "HTTP upload rejected with status " + std::to_string(http_status) };
    }

private:
    ImageUploadConfig config_;
};

}  // namespace

std::unique_ptr<ImageUploadTransport> CreateCurlImageUploadTransport(const ImageUploadConfig& config) {
    return std::make_unique<CurlImageUploadTransport>(config);
}

}  // namespace logistics::device
