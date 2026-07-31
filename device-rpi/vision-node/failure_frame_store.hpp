#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <string_view>

#include "vision_processing_config.hpp"

namespace logistics::vision {

class PendingWorkFrame final {
public:
    void Observe(const cv::Mat& frame, bool box_detected, bool work_pending);
    void Reset() noexcept;

    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] const cv::Mat& Frame() const noexcept;

private:
    cv::Mat frame_;
};

class FailureFrameStore final {
public:
    explicit FailureFrameStore(FailureFrameCaptureConfig config);

    [[nodiscard]] bool Store(const cv::Mat& frame, std::string_view work_id) noexcept;

private:
    [[nodiscard]] std::filesystem::path NextPath(std::string_view work_id);
    void Prune() noexcept;

    FailureFrameCaptureConfig config_;
    std::uint64_t sequence_{};
};

}  // namespace logistics::vision
