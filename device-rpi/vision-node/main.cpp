#include <cstddef>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/barcode.hpp>
#include <opencv2/videoio.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kEscapeKey = 27;
constexpr int kWaitKeyDelayMs = 1;
constexpr std::size_t kBarcodeCornerCount = 4;
constexpr std::size_t kEan13DigitCount = 13;
constexpr char kSupportedBarcodeType[] = "EAN_13";
const cv::Scalar kBarcodeBoxColor{ 0, 255, 0 };

const std::string kCameraPipeline =
    "libcamerasrc ! "
    "video/x-raw,width=640,height=480,framerate=30/1 ! "
    "videoconvert ! "
    "video/x-raw,format=BGR ! "
    "appsink drop=true max-buffers=1 sync=false";

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

    std::cout << "Camera started. Press q or Esc to exit.\n";

    cv::Mat frame;
    while (true) {
        if (!camera.read(frame) || frame.empty()) {
            std::cerr << "Failed to receive a camera frame.\n";
            return 1;
        }

        decoded_values.clear();
        decoded_types.clear();
        barcode_corners.clear();

        if (barcode_detector.detectAndDecodeWithType(frame, decoded_values, decoded_types, barcode_corners)) {
            for (std::size_t barcode_index = 0; barcode_index < decoded_values.size(); ++barcode_index) {
                const std::string& decoded_value = decoded_values[barcode_index];
                if (decoded_value.empty()) {
                    continue;
                }

                if (barcode_index >= decoded_types.size() || decoded_types[barcode_index] != kSupportedBarcodeType ||
                    decoded_value.size() != kEan13DigitCount) {
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

                for (std::size_t corner_index = 0; corner_index < kBarcodeCornerCount; ++corner_index) {
                    const cv::Point2f& start = barcode_corners[first_corner + corner_index];
                    const cv::Point2f& end = barcode_corners[first_corner + ((corner_index + 1) % kBarcodeCornerCount)];
                    cv::line(frame, start, end, kBarcodeBoxColor, 2);
                }

                const cv::Point2f& label_position = barcode_corners[first_corner];
                cv::putText(frame, label, label_position, cv::FONT_HERSHEY_SIMPLEX, 0.6, kBarcodeBoxColor, 2);
            }
        }

        cv::imshow(kWindowName, frame);

        const int key = cv::waitKey(kWaitKeyDelayMs) & 0xff;
        if (key == 'q' || key == kEscapeKey) {
            break;
        }
    }

    return 0;
}
