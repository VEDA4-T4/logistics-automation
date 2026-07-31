#include <cassert>
#include <chrono>
#include <filesystem>
#include <opencv2/core.hpp>
#include <string>

#include "failure_frame_store.hpp"

namespace {

namespace vision = logistics::vision;

[[nodiscard]] std::filesystem::path TemporaryDirectory() {
    return std::filesystem::temp_directory_path() /
           ("vision-failure-frames-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

[[nodiscard]] std::size_t JpegCount(const std::filesystem::path& directory) {
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        count += entry.is_regular_file() && entry.path().extension() == ".jpg" ? 1U : 0U;
    }
    return count;
}

void TestStorePrunesOldFrames() {
    const auto directory = TemporaryDirectory();
    vision::FailureFrameStore store({
        .enabled = true,
        .directory = directory,
        .maximum_frames = 2,
        .jpeg_quality = 80,
    });
    const cv::Mat frame(16, 16, CV_8UC3, cv::Scalar(20, 40, 60));
    assert(store.Store(frame, "work/one"));
    assert(store.Store(frame, "work-two"));
    assert(store.Store(frame, "work-three"));
    assert(JpegCount(directory) == 2);
    std::filesystem::remove_all(directory);
}

void TestDisabledStoreDoesNotCreateDirectory() {
    const auto directory = TemporaryDirectory();
    vision::FailureFrameStore store({
        .enabled = false,
        .directory = directory,
        .maximum_frames = 2,
        .jpeg_quality = 80,
    });
    const cv::Mat frame(4, 4, CV_8UC3, cv::Scalar{});
    assert(store.Store(frame, "work"));
    assert(!std::filesystem::exists(directory));
}

}  // namespace

int main() {
    TestStorePrunesOldFrames();
    TestDisabledStoreDoesNotCreateDirectory();
    return 0;
}
