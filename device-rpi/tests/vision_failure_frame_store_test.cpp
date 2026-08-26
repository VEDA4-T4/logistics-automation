#include <chrono>
#include <filesystem>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>

#include "failure_frame_store.hpp"

namespace {

namespace vision = logistics::vision;

void Require(const bool condition) {
    if (!condition) {
        throw std::runtime_error("vision failure frame store test condition failed");
    }
}

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
    Require(store.Store(frame, "work/one"));
    Require(store.Store(frame, "work-two"));
    Require(store.Store(frame, "work-three"));
    Require(JpegCount(directory) == 2);
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
    Require(store.Store(frame, "work"));
    Require(!std::filesystem::exists(directory));
}

void TestPendingWorkFrameRetainsLastFrameContainingABox() {
    vision::PendingWorkFrame pending;
    const cv::Mat box_frame(4, 4, CV_8UC1, cv::Scalar(42));
    const cv::Mat empty_scene(4, 4, CV_8UC1, cv::Scalar(7));

    pending.Observe(box_frame, true, false, true);
    pending.Observe(empty_scene, false, false, true);
    Require(!pending.Empty());
    Require(pending.Frame().at<unsigned char>(0, 0) == 42);

    pending.Reset();
    Require(pending.Empty());
}

void TestPendingWorkFrameLocksTheBarcodeFrame() {
    vision::PendingWorkFrame pending;
    const cv::Mat box_frame(4, 4, CV_8UC1, cv::Scalar(21));
    const cv::Mat barcode_frame(4, 4, CV_8UC1, cv::Scalar(42));
    const cv::Mat later_box_frame(4, 4, CV_8UC1, cv::Scalar(84));

    pending.Observe(box_frame, true, false, true);
    pending.Observe(barcode_frame, true, true, true);
    pending.Observe(later_box_frame, true, false, true);
    Require(!pending.Empty());
    Require(pending.Frame().at<unsigned char>(0, 0) == 42);

    pending.Reset();
    pending.Observe(later_box_frame, true, false, true);
    Require(pending.Frame().at<unsigned char>(0, 0) == 84);
}

void TestPendingWorkFrameRetainsBarcodeWithoutBox() {
    vision::PendingWorkFrame pending;
    const cv::Mat barcode_frame(4, 4, CV_8UC1, cv::Scalar(42));

    pending.Observe(barcode_frame, false, true, true);
    Require(!pending.Empty());
    Require(pending.Frame().at<unsigned char>(0, 0) == 42);
}

}  // namespace

int main() {
    TestStorePrunesOldFrames();
    TestDisabledStoreDoesNotCreateDirectory();
    TestPendingWorkFrameRetainsLastFrameContainingABox();
    TestPendingWorkFrameLocksTheBarcodeFrame();
    TestPendingWorkFrameRetainsBarcodeWithoutBox();
    return 0;
}
