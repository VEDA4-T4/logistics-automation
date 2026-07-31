#pragma once

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/objdetect/barcode.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vision_processing_config.hpp"

namespace logistics::vision {

struct DetectionDiagnostics final {
    bool barcode_region_detected{};
    bool barcode_decoded{};
    bool used_perspective_rectification{};
    bool used_contrast_enhancement{};
    bool used_super_resolution_for_detection{};
    bool used_super_resolution_for_decode{};
    bool super_resolution_failed{};
    double box_detection_ms{};
    double barcode_detection_ms{};
    double barcode_decode_ms{};
    double perspective_rectification_ms{};
    double contrast_enhancement_ms{};
    double super_resolution_ms{};
};

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
    DetectionDiagnostics diagnostics;
};

[[nodiscard]] bool IsRotatedRectangleInsideFrame(const cv::RotatedRect& rectangle, cv::Size frame_size) noexcept;

class DetectionModule final {
public:
    explicit DetectionModule(VisionProcessingConfig config = {});

    [[nodiscard]] DetectionResult Process(const cv::Mat& frame, bool allow_expensive_fallback = true);
    [[nodiscard]] cv::Mat SuperResolveForPreview(const cv::Mat& image);

private:
    [[nodiscard]] std::optional<DetectedBox> DetectStyrofoamBox(const cv::Mat& frame);
    [[nodiscard]] bool DetectBarcodeRegions(const cv::Mat& image, std::vector<cv::Point2f>& corners);
    void DecodeBarcodeRegions(const cv::Mat& image, const std::vector<cv::Point2f>& corners);
    [[nodiscard]] cv::Mat RectifyBarcode(const cv::Mat& image, const std::vector<cv::Point2f>& corners,
                                         std::size_t barcode_index) const;
    [[nodiscard]] cv::Mat CropBarcode(const cv::Mat& image, const std::vector<cv::Point2f>& corners,
                                      std::size_t barcode_index) const;
    [[nodiscard]] cv::Mat EnhanceContrast(const cv::Mat& image) const;
    [[nodiscard]] cv::Mat SuperResolve(const cv::Mat& image);
    [[nodiscard]] std::optional<cv::Mat> TrySuperResolve(const cv::Mat& image,
                                                         DetectionDiagnostics& diagnostics) noexcept;
    [[nodiscard]] bool IsSuperResolutionAllowed(const cv::Mat& image) const noexcept;
    void AppendDecodedBarcodes(const std::vector<cv::Point2f>& original_corners, const cv::Point2f& frame_offset,
                               std::vector<DetectedBarcode>& barcodes) const;
    [[nodiscard]] static bool IsValidEan13(std::string_view value);

    VisionProcessingConfig config_;
    cv::barcode::BarcodeDetector barcode_detector_;
    cv::dnn::Net super_resolution_net_;
    cv::Mat box_mask_;
    std::vector<std::string> decoded_values_;
    std::vector<std::string> decoded_types_;
    std::vector<cv::Point2f> barcode_corners_;
    std::size_t consecutive_barcode_failures_{};
};

}  // namespace logistics::vision
