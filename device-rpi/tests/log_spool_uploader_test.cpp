#include "logistics/device/log_spool_uploader.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

class RetryThenConfirmTransport final : public logistics::device::LogUploadTransport {
public:
    logistics::device::LogUploadResult Upload(const std::filesystem::path& file,
                                              const logistics::device::LogFileMetadata& metadata) override {
        assert(std::filesystem::file_size(file) == metadata.byte_size);
        ++calls;
        if (calls == 1) {
            return { logistics::device::UploadDisposition::kRetryableFailure, {}, "temporary failure" };
        }
        return { logistics::device::UploadDisposition::kConfirmed, metadata.sha256, "confirmed" };
    }

    int calls{};
};

}  // namespace

int main() {
    namespace device = logistics::device;
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("logistics-log-spool-test-" + unique);

    device::LogSpoolConfig config;
    config.device_id = "PI-VISION-01";
    config.spool_directory = root;
    config.rotate_bytes = 1024;
    config.maximum_spool_bytes = 4096;
    config.maximum_attempts = 3;
    config.initial_backoff = std::chrono::seconds(0);
    config.maximum_backoff = std::chrono::seconds(0);

    {
        auto transport = std::make_unique<RetryThenConfirmTransport>();
        auto* transport_view = transport.get();
        device::LogSpoolUploader uploader(config, std::move(transport));
        assert(uploader.Append("camera disconnected"));
        assert(uploader.RotateNow());
        assert(uploader.PendingCount() == 1);

        assert(uploader.ProcessPendingOnce());
        assert(transport_view->calls == 1);
        assert(uploader.PendingCount() == 1);

        assert(uploader.ProcessPendingOnce());
        assert(transport_view->calls == 2);
        assert(uploader.PendingCount() == 0);
    }

    {
        auto transport = std::make_unique<RetryThenConfirmTransport>();
        device::LogSpoolUploader recovered(config, std::move(transport));
        assert(recovered.Append("restart-safe log"));
        assert(recovered.RotateNow());
        assert(recovered.PendingCount() == 1);
    }
    {
        auto transport = std::make_unique<RetryThenConfirmTransport>();
        auto* transport_view = transport.get();
        device::LogSpoolUploader recovered(config, std::move(transport));
        assert(recovered.ProcessPendingOnce());
        assert(recovered.PendingCount() == 1);
        assert(recovered.ProcessPendingOnce());
        assert(transport_view->calls == 2);
        assert(recovered.PendingCount() == 0);
    }

    {
        const auto orphan = root / "orphan.pending.log";
        std::ofstream(orphan) << "log without metadata\n";
        auto transport = std::make_unique<RetryThenConfirmTransport>();
        device::LogSpoolUploader recovered(config, std::move(transport));
        assert(!recovered.ProcessPendingOnce());
        assert(!std::filesystem::exists(orphan));
        assert(std::filesystem::exists(root / "orphan.pending.log.orphaned"));
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return 0;
}
