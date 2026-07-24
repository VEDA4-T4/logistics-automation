#include "logistics/device/image_uploader.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "logistics/contracts/http_upload.hpp"

namespace logistics::device {

std::unique_ptr<ImageUploadTransport> CreateCurlImageUploadTransport(const ImageUploadConfig&) {
    return nullptr;
}

}  // namespace logistics::device

namespace {

namespace device = logistics::device;

class ConfirmingTransport final : public device::ImageUploadTransport {
public:
    device::ImageUploadResult Upload(const device::ImageUploadRequest& request) override {
        ++calls;
        last_request = request;
        return {
            .disposition = device::ImageUploadDisposition::kConfirmed,
            .upload_id = "42f8e6f1-1277-4748-9e5e-c41c7bf605f7",
            .path = "/uploads/images/42f8e6f1-1277-4748-9e5e-c41c7bf605f7.jpg",
            .checksum = request.sha256,
            .error = {},
        };
    }

    int calls{};
    device::ImageUploadRequest last_request;
};

device::ImageUploadConfig Config() {
    return {
        .endpoint_url = "https://server.example/api/v1/uploads/images",
        .bearer_token = "device-token",
        .ca_certificate = {},
        .request_timeout = std::chrono::seconds(10),
        .maximum_attempts = 1,
        .initial_backoff = std::chrono::seconds(1),
        .maximum_backoff = std::chrono::seconds(1),
        .allow_insecure_http = false,
    };
}

void TestConfirmedUpload() {
    auto transport = std::make_unique<ConfirmingTransport>();
    auto* transport_view = transport.get();
    const device::ImageUploader uploader(Config(), std::move(transport));
    const std::vector<std::uint8_t> image{ 0xff, 0xd8, 0xff, 0xd9 };
    const auto result = uploader.Upload("PI-VISION-01", "22a194c3-3e3c-410c-a329-7e8c4ebcac83", "UPLOAD-01",
                                        "2026-07-21T12:00:00Z", "capture.jpg", "image/jpeg", image);
    assert(result.IsConfirmed());
    assert(transport_view->calls == 1);
    assert(logistics::contracts::http::IsSha256(transport_view->last_request.sha256));
    assert(transport_view->last_request.bytes == image);
    assert(result.path.starts_with("/uploads/images/"));
}

void TestInvalidWorkIdIsRejectedBeforeTransport() {
    auto transport = std::make_unique<ConfirmingTransport>();
    auto* transport_view = transport.get();
    const device::ImageUploader uploader(Config(), std::move(transport));
    const std::vector<std::uint8_t> image{ 1, 2, 3 };
    const auto result = uploader.Upload("PI-VISION-01", "not-a-uuid", "UPLOAD-02", "2026-07-21T12:00:00Z",
                                        "capture.jpg", "image/jpeg", image);
    assert(!result.IsConfirmed());
    assert(result.disposition == device::ImageUploadDisposition::kPermanentFailure);
    assert(transport_view->calls == 0);
}

}  // namespace

int main() {
    TestConfirmedUpload();
    TestInvalidWorkIdIsRejectedBeforeTransport();
    return 0;
}
