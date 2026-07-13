#pragma once

#include <opencv2/core.hpp>
#if __has_include(<opencv2/objdetect/barcode.hpp>)
#include <opencv2/objdetect/barcode.hpp>
#elif __has_include(<opencv2/barcode.hpp>)
#include <opencv2/barcode.hpp>
#else
#error "OpenCV barcode detector header was not found"
#endif
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logistics::vision {

struct DetectedBox {
    cv::Rect roi;
    cv::RotatedRect outline;
};

struct DetectedBarcode {
    std::string type;
    std::string value;
    std::vector<cv::Point2f> corners;
};

struct DetectionResult {
    std::optional<DetectedBox> box;
    std::vector<DetectedBarcode> barcodes;
};

class DetectionModule final {
public:
    [[nodiscard]] DetectionResult Process(const cv::Mat& frame);

private:
    [[nodiscard]] std::optional<DetectedBox> DetectStyrofoamBox(const cv::Mat& frame);
    [[nodiscard]] static bool IsValidEan13(std::string_view value);

    cv::barcode::BarcodeDetector barcode_detector_;
    cv::Mat box_mask_;
    std::vector<std::string> decoded_values_;
    std::vector<std::string> decoded_types_;
    std::vector<cv::Point2f> barcode_corners_;
};

}  // namespace logistics::vision
