#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/barcode.hpp>
#include <opencv2/videoio.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kEscapeKey = 27;
constexpr int kWaitKeyDelayMs = 1;
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
const cv::Scalar kBoxOutlineColor{ 255, 128, 0 };
const cv::Scalar kBarcodeBoxColor{ 0, 255, 0 };
const cv::Scalar kNotFoundColor{ 0, 0, 255 };
const cv::Mat kBoxCloseKernel =
    cv::getStructuringElement(cv::MORPH_RECT, cv::Size(kBoxCloseKernelSize, kBoxCloseKernelSize));
const cv::Mat kBoxOpenKernel =
    cv::getStructuringElement(cv::MORPH_RECT, cv::Size(kBoxOpenKernelSize, kBoxOpenKernelSize));

const std::string kCameraPipeline =
    "libcamerasrc ! "
    "video/x-raw,width=1280,height=720,framerate=30/1 ! "
    "videoconvert ! "
    "video/x-raw,format=BGR ! "
    "appsink drop=true max-buffers=1 sync=false";

struct DetectedBox {
    cv::Rect roi;
    cv::RotatedRect outline;
};

std::optional<DetectedBox> DetectStyrofoamBox(const cv::Mat& frame, cv::Mat& box_mask) {
    cv::Mat hsv_frame;
    cv::cvtColor(frame, hsv_frame, cv::COLOR_BGR2HSV);
    cv::inRange(hsv_frame, kBoxMaskLowerHsv, kBoxMaskUpperHsv, box_mask);

    cv::morphologyEx(box_mask, box_mask, cv::MORPH_CLOSE, kBoxCloseKernel);
    cv::morphologyEx(box_mask, box_mask, cv::MORPH_OPEN, kBoxOpenKernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(box_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

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

void DrawDetectedBox(cv::Mat& frame, const DetectedBox& detected_box) {
    cv::Point2f corners[kBarcodeCornerCount];
    detected_box.outline.points(corners);

    for (std::size_t corner_index = 0; corner_index < kBarcodeCornerCount; ++corner_index) {
        const cv::Point2f& start = corners[corner_index];
        const cv::Point2f& end = corners[(corner_index + 1) % kBarcodeCornerCount];
        cv::line(frame, start, end, kBoxOutlineColor, 2);
    }

    const cv::Point center{ cvRound(detected_box.outline.center.x), cvRound(detected_box.outline.center.y) };
    const cv::Point label_position{ detected_box.roi.x, std::max(25, detected_box.roi.y - 8) };
    cv::circle(frame, center, 4, kBoxOutlineColor, cv::FILLED);
    cv::putText(frame, "BOX", label_position, cv::FONT_HERSHEY_SIMPLEX, 0.7, kBoxOutlineColor, 2);
}

bool IsValidEan13(std::string_view value) {
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

}  // namespace

int main() {
    cv::VideoCapture camera(kCameraPipeline, cv::CAP_GSTREAMER);
    if (!camera.isOpened()) {
        std::cerr << "Failed to open the Raspberry Pi camera GStreamer pipeline.\n";
        return 1;
    }

    constexpr char kWindowName[] = "vision-node camera";
    cv::namedWindow(kWindowName, cv::WINDOW_AUTOSIZE);

    cv::barcode::BarcodeDetector barcode_detector;
    std::unordered_set<std::string> reported_barcodes;
    std::vector<std::string> decoded_values;
    std::vector<std::string> decoded_types;
    std::vector<cv::Point2f> barcode_corners;
    cv::Mat box_mask;

    std::cout << "Camera started. Press q or Esc to exit.\n";

    cv::Mat frame;
    int exit_code = 0;
    while (true) {
        if (!camera.read(frame) || frame.empty()) {
            std::cerr << "Failed to receive a camera frame.\n";
            exit_code = 1;
            break;
        }

        const std::optional<DetectedBox> detected_box = DetectStyrofoamBox(frame, box_mask);
        if (detected_box.has_value()) {
            const cv::Mat box_roi = frame(detected_box->roi).clone();
            DrawDetectedBox(frame, *detected_box);

            decoded_values.clear();
            decoded_types.clear();
            barcode_corners.clear();

            barcode_detector.detectAndDecodeWithType(box_roi, decoded_values, decoded_types, barcode_corners);

            for (std::size_t barcode_index = 0; barcode_index < decoded_values.size(); ++barcode_index) {
                const std::string& decoded_value = decoded_values[barcode_index];
                if (decoded_value.empty()) {
                    continue;
                }

                if (barcode_index >= decoded_types.size() || decoded_types[barcode_index] != kSupportedBarcodeType ||
                    !IsValidEan13(decoded_value)) {
                    continue;
                }

                const std::string label = decoded_types[barcode_index] + ": " + decoded_value;

                if (reported_barcodes.insert(decoded_value).second) {
                    std::cout << "Barcode detected: " << label << '\n';
                }

                const std::size_t first_corner = barcode_index * kBarcodeCornerCount;
                if (barcode_corners.size() < first_corner + kBarcodeCornerCount) {
                    continue;
                }

                const cv::Point2f roi_offset{ static_cast<float>(detected_box->roi.x),
                                              static_cast<float>(detected_box->roi.y) };
                for (std::size_t corner_index = 0; corner_index < kBarcodeCornerCount; ++corner_index) {
                    const cv::Point2f start = barcode_corners[first_corner + corner_index] + roi_offset;
                    const cv::Point2f end =
                        barcode_corners[first_corner + ((corner_index + 1) % kBarcodeCornerCount)] + roi_offset;
                    cv::line(frame, start, end, kBarcodeBoxColor, 2);
                }

                const cv::Point2f label_position = barcode_corners[first_corner] + roi_offset;
                cv::putText(frame, label, label_position, cv::FONT_HERSHEY_SIMPLEX, 0.6, kBarcodeBoxColor, 2);
            }
        } else {
            cv::putText(frame, "BOX: not found", cv::Point(20, 35), cv::FONT_HERSHEY_SIMPLEX, 0.8, kNotFoundColor, 2);
        }

        cv::imshow(kWindowName, frame);

        const int key = cv::waitKey(kWaitKeyDelayMs) & 0xff;
        if (key == 'q' || key == kEscapeKey) {
            break;
        }
    }

    camera.release();
    cv::destroyAllWindows();
    return exit_code;
}
