#include "failure_frame_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace logistics::vision {
namespace {

[[nodiscard]] std::string SafeFileComponent(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(std::isalnum(character) != 0 || character == '-' || character == '_' ? character : '_');
    }
    return result.empty() ? "unknown-work" : result;
}

}  // namespace

void PendingWorkFrame::Observe(const cv::Mat& frame, const bool box_detected, const bool work_pending) {
    if (box_detected && work_pending && !frame.empty()) {
        frame_ = frame.clone();
    }
}

void PendingWorkFrame::Reset() noexcept {
    frame_.release();
}

bool PendingWorkFrame::Empty() const noexcept {
    return frame_.empty();
}

const cv::Mat& PendingWorkFrame::Frame() const noexcept {
    return frame_;
}

FailureFrameStore::FailureFrameStore(FailureFrameCaptureConfig config) : config_(std::move(config)) {}

bool FailureFrameStore::Store(const cv::Mat& frame, const std::string_view work_id) noexcept {
    if (!config_.enabled || frame.empty()) {
        return !config_.enabled;
    }
    try {
        std::error_code error;
        std::filesystem::create_directories(config_.directory, error);
        if (error) {
            return false;
        }

        std::vector<unsigned char> jpeg;
        if (!cv::imencode(".jpg", frame, jpeg, { cv::IMWRITE_JPEG_QUALITY, config_.jpeg_quality })) {
            return false;
        }
        const std::filesystem::path target = NextPath(work_id);
        std::filesystem::path temporary = target;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
            if (!output) {
                output.close();
                std::filesystem::remove(temporary, error);
                return false;
            }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
        Prune();
        return true;
    } catch (...) {
        return false;
    }
}

std::filesystem::path FailureFrameStore::NextPath(const std::string_view work_id) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return config_.directory / (std::to_string(milliseconds) + '-' + std::to_string(sequence_++) + '-' +
                                SafeFileComponent(work_id) + ".jpg");
}

void FailureFrameStore::Prune() noexcept {
    try {
        std::error_code error;
        std::vector<std::filesystem::directory_entry> frames;
        for (std::filesystem::directory_iterator iterator(config_.directory, error), end; !error && iterator != end;
             iterator.increment(error)) {
            if (iterator->is_regular_file(error) && iterator->path().extension() == ".jpg") {
                frames.push_back(*iterator);
            }
        }
        if (error || frames.size() <= config_.maximum_frames) {
            return;
        }
        std::ranges::sort(frames, [](const auto& left, const auto& right) {
            std::error_code left_error;
            std::error_code right_error;
            const auto left_time = left.last_write_time(left_error);
            const auto right_time = right.last_write_time(right_error);
            if (left_error || right_error || left_time == right_time) {
                return left.path().filename() < right.path().filename();
            }
            return left_time < right_time;
        });
        const std::size_t remove_count = frames.size() - config_.maximum_frames;
        for (std::size_t index = 0; index < remove_count; ++index) {
            std::filesystem::remove(frames[index].path(), error);
            error.clear();
        }
    } catch (...) {
    }
}

}  // namespace logistics::vision
