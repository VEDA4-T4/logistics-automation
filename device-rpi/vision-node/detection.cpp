#include "detection.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace logistics::vision {
namespace {

constexpr int kBoxCloseKernelSize = 11;
constexpr int kBoxOpenKernelSize = 5;
constexpr std::size_t kBarcodeCornerCount = 4;
constexpr std::size_t kEan13DigitCount = 13;
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

}  // namespace

DetectionResult DetectionModule::Process(const cv::Mat& frame) {
    DetectionResult result;
    result.box = DetectStyrofoamBox(frame);
    if (!result.box.has_value()) {
        return result;
    }

    decoded_values_.clear();
    decoded_types_.clear();
    barcode_corners_.clear();

    const cv::Mat box_roi = frame(result.box->roi);
    barcode_detector_.detectAndDecodeWithType(box_roi, decoded_values_, decoded_types_, barcode_corners_);

    for (std::size_t barcode_index = 0; barcode_index < decoded_values_.size(); ++barcode_index) {
        if (barcode_index >= decoded_types_.size() || decoded_types_[barcode_index] != kSupportedBarcodeType ||
            !IsValidEan13(decoded_values_[barcode_index])) {
            continue;
        }

        DetectedBarcode barcode;
        barcode.type = decoded_types_[barcode_index];
        barcode.value = decoded_values_[barcode_index];

        const std::size_t first_corner = barcode_index * kBarcodeCornerCount;
        if (barcode_corners_.size() >= first_corner + kBarcodeCornerCount) {
            const cv::Point2f roi_offset{ static_cast<float>(result.box->roi.x),
                                          static_cast<float>(result.box->roi.y) };
            barcode.corners.reserve(kBarcodeCornerCount);
            for (std::size_t corner_index = 0; corner_index < kBarcodeCornerCount; ++corner_index) {
                barcode.corners.push_back(barcode_corners_[first_corner + corner_index] + roi_offset);
            }
        }

        result.barcodes.push_back(std::move(barcode));
    }

    return result;
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
        if (width <= 0.0 || height <= 0.0) {
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
