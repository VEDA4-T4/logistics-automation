#include "detection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <utility>

namespace logistics::vision {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBoxCloseKernelSize = 11;
constexpr int kBoxOpenKernelSize = 5;
constexpr std::size_t kBarcodeCornerCount = 4;
constexpr std::size_t kEan13DigitCount = 13;
constexpr int kBarcodeQuietZonePixels = 16;
constexpr int kMinimumRectifiedBarcodeDimension = 32;
constexpr char kSupportedBarcodeType[] = "EAN_13";
constexpr double kMinimumBoxAreaPixels = 2500.0;
constexpr double kMaximumBoxFrameAreaRatio = 0.8;
constexpr double kMinimumBoxRectangularity = 0.65;
constexpr double kMaximumBoxAspectRatio = 1.4;
const cv::Scalar kBoxMaskLowerHsv{ 0, 0, 160 };
const cv::Scalar kBoxMaskUpperHsv{ 180, 70, 255 };
const cv::Mat kBoxCloseKernel =
    cv::getStructuringElement(cv::MORPH_RECT, cv::Size(kBoxCloseKernelSize, kBoxCloseKernelSize));
const cv::Mat kBoxOpenKernel =
    cv::getStructuringElement(cv::MORPH_RECT, cv::Size(kBoxOpenKernelSize, kBoxOpenKernelSize));

template <typename Duration>
double ToMilliseconds(const Duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double PointDistance(const cv::Point2f& first, const cv::Point2f& second) {
    return cv::norm(first - second);
}

}  // namespace

bool IsRotatedRectangleInsideFrame(const cv::RotatedRect& rectangle, const cv::Size frame_size) noexcept {
    if (frame_size.width <= 0 || frame_size.height <= 0) {
        return false;
    }
    std::array<cv::Point2f, 4> corners{};
    rectangle.points(corners.data());
    return std::ranges::all_of(corners, [frame_size](const cv::Point2f& corner) {
        return std::isfinite(corner.x) && std::isfinite(corner.y) && corner.x >= 0.0F && corner.y >= 0.0F &&
               corner.x < static_cast<float>(frame_size.width) && corner.y < static_cast<float>(frame_size.height);
    });
}

DetectionModule::DetectionModule(VisionProcessingConfig config) : config_(std::move(config)) {
    if (!config_.IsValid()) {
        throw std::invalid_argument("invalid vision processing configuration");
    }
    if (config_.super_resolution_enabled && config_.super_resolution_backend == SuperResolutionBackend::kFsrcnn) {
        super_resolution_net_ = cv::dnn::readNetFromTensorflow(config_.super_resolution_model_path.string());
        if (super_resolution_net_.empty()) {
            throw std::runtime_error("failed to load FSRCNN model: " + config_.super_resolution_model_path.string());
        }
    }
}

DetectionResult DetectionModule::Process(const cv::Mat& frame, const bool allow_expensive_fallback) {
    DetectionResult result;
    const auto box_started = Clock::now();
    result.box = DetectStyrofoamBox(frame);
    result.diagnostics.box_detection_ms = ToMilliseconds(Clock::now() - box_started);
    if (!result.box.has_value()) {
        consecutive_barcode_failures_ = 0;
        return result;
    }

    const cv::Mat box_roi = frame(result.box->roi);
    std::vector<cv::Point2f> detected_corners;
    const std::size_t next_consecutive_barcode_failures = consecutive_barcode_failures_ + 1;
    const bool reached_failure_threshold =
        next_consecutive_barcode_failures >= static_cast<std::size_t>(config_.failure_frames_before_super_resolution);
    const bool detected =
        DetectBarcodeRegionsWithFallback(box_roi, allow_expensive_fallback, reached_failure_threshold,
                                         consecutive_barcode_failures_, detected_corners, result.diagnostics);

    result.diagnostics.barcode_region_detected = detected;
    if (!detected) {
        return result;
    }

    const auto decode_started = Clock::now();
    DecodeBarcodeRegions(box_roi, detected_corners);
    result.diagnostics.barcode_decode_ms = ToMilliseconds(Clock::now() - decode_started);

    const cv::Point2f frame_offset{ static_cast<float>(result.box->roi.x), static_cast<float>(result.box->roi.y) };
    AppendDecodedBarcodes(detected_corners, frame_offset, result.barcodes);
    if (!result.barcodes.empty()) {
        consecutive_barcode_failures_ = 0;
        result.diagnostics.barcode_decoded = true;
        return result;
    }

    if (!allow_expensive_fallback || !reached_failure_threshold || !config_.barcode_decode_fallback) {
        return result;
    }

    const std::size_t barcode_count = detected_corners.size() / kBarcodeCornerCount;
    for (std::size_t barcode_index = 0; barcode_index < barcode_count && result.barcodes.empty(); ++barcode_index) {
        const auto first_corner =
            detected_corners.begin() + static_cast<std::ptrdiff_t>(barcode_index * kBarcodeCornerCount);
        const std::vector<cv::Point2f> selected_corners(
            first_corner, first_corner + static_cast<std::ptrdiff_t>(kBarcodeCornerCount));
        cv::Mat decode_roi = PrepareBarcodeDecodeRoi(box_roi, detected_corners, barcode_index, result.diagnostics);
        if (decode_roi.empty()) {
            continue;
        }
        DecodeBarcodeCandidate(decode_roi, selected_corners, frame_offset, result.barcodes, result.diagnostics);
    }

    if (!result.barcodes.empty()) {
        consecutive_barcode_failures_ = 0;
        result.diagnostics.barcode_decoded = true;
    }
    return result;
}

cv::Mat DetectionModule::SuperResolveForPreview(const cv::Mat& image) {
    if (image.empty()) {
        throw std::invalid_argument("super-resolution preview image must not be empty");
    }
    if (!config_.super_resolution_enabled) {
        throw std::logic_error("super-resolution preview requires an enabled backend");
    }
    return SuperResolve(image);
}

bool DetectionModule::DetectBarcodeRegions(const cv::Mat& image, std::vector<cv::Point2f>& corners) {
    corners.clear();
    return barcode_detector_.detect(image, corners) && corners.size() >= kBarcodeCornerCount;
}

bool DetectionModule::DetectBarcodeRegionsWithFallback(const cv::Mat& box_roi, const bool allow_expensive_fallback,
                                                       const bool reached_failure_threshold,
                                                       std::size_t& consecutive_barcode_failures,
                                                       std::vector<cv::Point2f>& corners,
                                                       DetectionDiagnostics& diagnostics) {
    const auto detection_started = Clock::now();
    bool detected = DetectBarcodeRegions(box_roi, corners);
    diagnostics.barcode_detection_ms = ToMilliseconds(Clock::now() - detection_started);
    ++consecutive_barcode_failures;

    if (!detected && allow_expensive_fallback && reached_failure_threshold && config_.super_resolution_enabled &&
        config_.barcode_detection_fallback && IsSuperResolutionAllowed(box_roi)) {
        const auto sr_started = Clock::now();
        const std::optional<cv::Mat> super_resolved_box = TrySuperResolve(box_roi, diagnostics);
        diagnostics.super_resolution_ms += ToMilliseconds(Clock::now() - sr_started);
        diagnostics.used_super_resolution_for_detection = true;
        if (!super_resolved_box.has_value()) {
            return false;
        }

        std::vector<cv::Point2f> super_resolved_corners;
        const auto retry_started = Clock::now();
        detected = DetectBarcodeRegions(*super_resolved_box, super_resolved_corners);
        diagnostics.barcode_detection_ms += ToMilliseconds(Clock::now() - retry_started);
        if (detected) {
            const float inverse_scale = 1.0F / static_cast<float>(config_.super_resolution_scale);
            corners.reserve(super_resolved_corners.size());
            for (const cv::Point2f& corner : super_resolved_corners) {
                corners.push_back(corner * inverse_scale);
            }
        }
    }

    return detected;
}

void DetectionModule::DecodeBarcodeRegions(const cv::Mat& image, const std::vector<cv::Point2f>& corners) {
    decoded_values_.clear();
    decoded_types_.clear();
    barcode_corners_.clear();
    static_cast<void>(barcode_detector_.decodeWithType(image, corners, decoded_values_, decoded_types_));
}

cv::Mat DetectionModule::PrepareBarcodeDecodeRoi(const cv::Mat& box_roi, const std::vector<cv::Point2f>& corners,
                                                 const std::size_t barcode_index,
                                                 DetectionDiagnostics& diagnostics) const {
    if (!config_.perspective_rectification) {
        return CropBarcode(box_roi, corners, barcode_index);
    }

    const auto rectify_started = Clock::now();
    cv::Mat decode_roi = RectifyBarcode(box_roi, corners, barcode_index);
    diagnostics.perspective_rectification_ms += ToMilliseconds(Clock::now() - rectify_started);
    diagnostics.used_perspective_rectification = !decode_roi.empty();
    return decode_roi;
}

void DetectionModule::DecodeBarcodeCandidate(const cv::Mat& decode_roi,
                                             const std::vector<cv::Point2f>& selected_corners,
                                             const cv::Point2f& frame_offset, std::vector<DetectedBarcode>& barcodes,
                                             DetectionDiagnostics& diagnostics) {
    cv::Mat enhanced_decode_roi = decode_roi;
    if (config_.contrast_enhancement) {
        const auto contrast_started = Clock::now();
        enhanced_decode_roi = EnhanceContrast(enhanced_decode_roi);
        diagnostics.contrast_enhancement_ms += ToMilliseconds(Clock::now() - contrast_started);
        diagnostics.used_contrast_enhancement = true;
    }

    decoded_values_.clear();
    decoded_types_.clear();
    barcode_corners_.clear();
    const auto enhanced_decode_started = Clock::now();
    barcode_detector_.detectAndDecodeWithType(enhanced_decode_roi, decoded_values_, decoded_types_, barcode_corners_);
    diagnostics.barcode_decode_ms += ToMilliseconds(Clock::now() - enhanced_decode_started);
    AppendDecodedBarcodes(selected_corners, frame_offset, barcodes);
    if (!barcodes.empty()) {
        return;
    }

    if (!config_.super_resolution_enabled || !IsSuperResolutionAllowed(enhanced_decode_roi)) {
        return;
    }
    const auto sr_started = Clock::now();
    const std::optional<cv::Mat> super_resolved_barcode = TrySuperResolve(enhanced_decode_roi, diagnostics);
    diagnostics.super_resolution_ms += ToMilliseconds(Clock::now() - sr_started);
    diagnostics.used_super_resolution_for_decode = true;
    if (!super_resolved_barcode.has_value()) {
        return;
    }

    decoded_values_.clear();
    decoded_types_.clear();
    barcode_corners_.clear();
    const auto sr_decode_started = Clock::now();
    barcode_detector_.detectAndDecodeWithType(*super_resolved_barcode, decoded_values_, decoded_types_,
                                              barcode_corners_);
    diagnostics.barcode_decode_ms += ToMilliseconds(Clock::now() - sr_decode_started);
    AppendDecodedBarcodes(selected_corners, frame_offset, barcodes);
}

cv::Mat DetectionModule::RectifyBarcode(const cv::Mat& image, const std::vector<cv::Point2f>& corners,
                                        const std::size_t barcode_index) const {
    const std::size_t first_corner = barcode_index * kBarcodeCornerCount;
    if (corners.size() < first_corner + kBarcodeCornerCount) {
        return {};
    }

    // OpenCV barcode points are bottom-left, top-left, top-right, bottom-right.
    const std::array<cv::Point2f, kBarcodeCornerCount> source{ corners[first_corner], corners[first_corner + 1],
                                                               corners[first_corner + 2], corners[first_corner + 3] };
    const int width = cvRound(std::max(PointDistance(source[1], source[2]), PointDistance(source[0], source[3])));
    const int height = cvRound(std::max(PointDistance(source[0], source[1]), PointDistance(source[3], source[2])));
    if (width < kMinimumRectifiedBarcodeDimension || height < kMinimumRectifiedBarcodeDimension) {
        return {};
    }

    const int output_width = width + kBarcodeQuietZonePixels * 2;
    const int output_height = height + kBarcodeQuietZonePixels * 2;
    const std::array<cv::Point2f, kBarcodeCornerCount> destination{
        cv::Point2f{ static_cast<float>(kBarcodeQuietZonePixels),
                     static_cast<float>(height + kBarcodeQuietZonePixels) },
        cv::Point2f{ static_cast<float>(kBarcodeQuietZonePixels), static_cast<float>(kBarcodeQuietZonePixels) },
        cv::Point2f{ static_cast<float>(width + kBarcodeQuietZonePixels), static_cast<float>(kBarcodeQuietZonePixels) },
        cv::Point2f{ static_cast<float>(width + kBarcodeQuietZonePixels),
                     static_cast<float>(height + kBarcodeQuietZonePixels) }
    };
    const cv::Mat transform = cv::getPerspectiveTransform(source.data(), destination.data());
    cv::Mat rectified;
    cv::warpPerspective(image, rectified, transform, cv::Size(output_width, output_height), cv::INTER_CUBIC,
                        cv::BORDER_CONSTANT, cv::Scalar::all(255));
    return rectified;
}

cv::Mat DetectionModule::CropBarcode(const cv::Mat& image, const std::vector<cv::Point2f>& corners,
                                     const std::size_t barcode_index) const {
    const std::size_t first_corner = barcode_index * kBarcodeCornerCount;
    if (corners.size() < first_corner + kBarcodeCornerCount) {
        return {};
    }

    const auto first = corners.begin() + static_cast<std::ptrdiff_t>(first_corner);
    const std::vector<cv::Point2f> selected(first, first + static_cast<std::ptrdiff_t>(kBarcodeCornerCount));
    cv::Rect bounds = cv::boundingRect(selected);
    if (bounds.width < kMinimumRectifiedBarcodeDimension || bounds.height < kMinimumRectifiedBarcodeDimension) {
        return {};
    }
    bounds.x -= kBarcodeQuietZonePixels;
    bounds.y -= kBarcodeQuietZonePixels;
    bounds.width += kBarcodeQuietZonePixels * 2;
    bounds.height += kBarcodeQuietZonePixels * 2;
    bounds &= cv::Rect{ 0, 0, image.cols, image.rows };
    return bounds.empty() ? cv::Mat{} : image(bounds).clone();
}

cv::Mat DetectionModule::EnhanceContrast(const cv::Mat& image) const {
    cv::Mat gray;
    if (image.channels() == 1) {
        gray = image;
    } else {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    }
    cv::Mat enhanced;
    const cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(gray, enhanced);
    cv::Mat blurred;
    cv::GaussianBlur(enhanced, blurred, cv::Size{}, 1.0);
    cv::addWeighted(enhanced, 1.5, blurred, -0.5, 0.0, enhanced);
    return enhanced;
}

cv::Mat DetectionModule::SuperResolve(const cv::Mat& image) {
    if (config_.super_resolution_backend == SuperResolutionBackend::kBicubic) {
        cv::Mat upscaled;
        cv::resize(image, upscaled, cv::Size{}, static_cast<double>(config_.super_resolution_scale),
                   static_cast<double>(config_.super_resolution_scale), cv::INTER_CUBIC);
        return upscaled;
    }

    cv::Mat bgr;
    if (image.channels() == 1) {
        cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
    } else {
        bgr = image;
    }
    cv::Mat ycrcb;
    cv::cvtColor(bgr, ycrcb, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> channels;
    cv::split(ycrcb, channels);

    super_resolution_net_.setInput(cv::dnn::blobFromImage(channels[0], 1.0 / 255.0));
    cv::Mat output = super_resolution_net_.forward();
    if (output.dims != 4 || output.size[0] != 1 || output.size[1] != 1) {
        throw std::runtime_error("FSRCNN output must have shape [1, 1, height, width]");
    }
    if (output.size[2] != image.rows * config_.super_resolution_scale ||
        output.size[3] != image.cols * config_.super_resolution_scale) {
        throw std::runtime_error("FSRCNN model output scale does not match super_resolution_scale");
    }
    cv::Mat luminance(output.size[2], output.size[3], CV_32F, output.ptr<float>());
    luminance.convertTo(luminance, CV_8U, 255.0);
    cv::resize(channels[1], channels[1], luminance.size(), 0.0, 0.0, cv::INTER_CUBIC);
    cv::resize(channels[2], channels[2], luminance.size(), 0.0, 0.0, cv::INTER_CUBIC);
    channels[0] = luminance;
    cv::merge(channels, ycrcb);
    cv::Mat upscaled;
    cv::cvtColor(ycrcb, upscaled, cv::COLOR_YCrCb2BGR);
    return upscaled;
}

std::optional<cv::Mat> DetectionModule::TrySuperResolve(const cv::Mat& image,
                                                        DetectionDiagnostics& diagnostics) noexcept {
    try {
        return SuperResolve(image);
    } catch (const cv::Exception&) {
        diagnostics.super_resolution_failed = true;
        return std::nullopt;
    } catch (const std::exception&) {
        diagnostics.super_resolution_failed = true;
        return std::nullopt;
    }
}

bool DetectionModule::IsSuperResolutionAllowed(const cv::Mat& image) const noexcept {
    return !image.empty() && image.total() <= config_.maximum_super_resolution_input_pixels;
}

void DetectionModule::AppendDecodedBarcodes(const std::vector<cv::Point2f>& original_corners,
                                            const cv::Point2f& frame_offset,
                                            std::vector<DetectedBarcode>& barcodes) const {
    for (std::size_t barcode_index = 0; barcode_index < decoded_values_.size(); ++barcode_index) {
        if (barcode_index >= decoded_types_.size() || decoded_types_[barcode_index] != kSupportedBarcodeType ||
            !IsValidEan13(decoded_values_[barcode_index])) {
            continue;
        }
        if (std::any_of(barcodes.begin(), barcodes.end(), [&](const DetectedBarcode& barcode) {
                return barcode.value == decoded_values_[barcode_index];
            })) {
            continue;
        }

        DetectedBarcode barcode{ .type = decoded_types_[barcode_index], .value = decoded_values_[barcode_index] };
        const std::size_t first_corner = barcode_index * kBarcodeCornerCount;
        if (original_corners.size() >= first_corner + kBarcodeCornerCount) {
            barcode.corners.reserve(kBarcodeCornerCount);
            for (std::size_t corner_index = 0; corner_index < kBarcodeCornerCount; ++corner_index) {
                barcode.corners.push_back(original_corners[first_corner + corner_index] + frame_offset);
            }
        }
        barcodes.push_back(std::move(barcode));
    }
}

std::optional<DetectedBox> DetectionModule::DetectStyrofoamBox(const cv::Mat& frame) {
    cv::Mat hsv_frame;
    cv::cvtColor(frame, hsv_frame, cv::COLOR_BGR2HSV);
    cv::inRange(hsv_frame, kBoxMaskLowerHsv, kBoxMaskUpperHsv, box_mask_);
    cv::morphologyEx(box_mask_, box_mask_, cv::MORPH_CLOSE, kBoxCloseKernel);
    cv::morphologyEx(box_mask_, box_mask_, cv::MORPH_OPEN, kBoxOpenKernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(box_mask_, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    const double maximum_box_area = static_cast<double>(frame.total()) * kMaximumBoxFrameAreaRatio;
    const cv::Rect frame_bounds{ 0, 0, frame.cols, frame.rows };
    double largest_box_area = 0.0;
    std::optional<DetectedBox> detected_box;

    for (const std::vector<cv::Point>& contour : contours) {
        const double contour_area = cv::contourArea(contour);
        if (contour_area < kMinimumBoxAreaPixels || contour_area > maximum_box_area) {
            continue;
        }

        const cv::RotatedRect outline = cv::minAreaRect(contour);
        const double width = static_cast<double>(outline.size.width);
        const double height = static_cast<double>(outline.size.height);
        if (width <= 0.0 || height <= 0.0 || !IsRotatedRectangleInsideFrame(outline, frame.size())) {
            continue;
        }

        const double aspect_ratio = std::max(width, height) / std::min(width, height);
        const double rectangularity = contour_area / (width * height);
        if (aspect_ratio > kMaximumBoxAspectRatio || rectangularity < kMinimumBoxRectangularity) {
            continue;
        }

        const cv::Rect roi = cv::boundingRect(contour) & frame_bounds;
        if (roi.empty() || contour_area <= largest_box_area) {
            continue;
        }

        largest_box_area = contour_area;
        detected_box = DetectedBox{ roi, outline };
    }

    return detected_box;
}

bool DetectionModule::IsValidEan13(const std::string_view value) {
    if (value.size() != kEan13DigitCount || !std::all_of(value.begin(), value.end(), [](const char character) {
            return std::isdigit(static_cast<unsigned char>(character)) != 0;
        })) {
        return false;
    }

    int checksum_sum = 0;
    for (std::size_t index = 0; index + 1 < value.size(); ++index) {
        const int digit = value[index] - '0';
        checksum_sum += digit * (index % 2 == 0 ? 1 : 3);
    }

    const int expected_check_digit = (10 - (checksum_sum % 10)) % 10;
    return expected_check_digit == value.back() - '0';
}

}  // namespace logistics::vision
