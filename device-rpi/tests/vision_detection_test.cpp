#include <array>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "detection.hpp"

namespace {

namespace vision = logistics::vision;

void Require(const bool condition) {
    if (!condition) {
        throw std::runtime_error("vision detection test condition failed");
    }
}

void TestRotatedRectangleMustRemainInsideFrame() {
    const cv::Size frame_size{ 1920, 1080 };
    Require(vision::IsRotatedRectangleInsideFrame(cv::RotatedRect({ 960.0F, 540.0F }, { 400.0F, 200.0F }, 20.0F),
                                                  frame_size));
    Require(!vision::IsRotatedRectangleInsideFrame(cv::RotatedRect({ 20.0F, 20.0F }, { 100.0F, 100.0F }, 20.0F),
                                                   frame_size));
    Require(!vision::IsRotatedRectangleInsideFrame(cv::RotatedRect({ 1900.0F, 1060.0F }, { 100.0F, 100.0F }, -20.0F),
                                                   frame_size));
}

cv::Mat MakeBarcodeOnlyFrame() {
    constexpr std::array<std::string_view, 10> kLeft{
        "0001101", "0011001", "0010011", "0111101", "0100011", "0110001", "0101111", "0111011", "0110111", "0001011",
    };
    constexpr std::array<std::string_view, 10> kEven{
        "0100111", "0110011", "0011011", "0100001", "0011101", "0111001", "0000101", "0010001", "0001001", "0010111",
    };
    constexpr std::array<std::string_view, 10> kRight{
        "1110010", "1100110", "1101100", "1000010", "1011100", "1001110", "1010000", "1000100", "1001000", "1110100",
    };
    constexpr std::array<std::string_view, 10> kParity{
        "LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG", "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL",
    };
    constexpr std::string_view kBarcode = "5901234123457";
    constexpr int kModuleWidth = 4;
    constexpr int kQuietModules = 15;
    constexpr int kBarcodeHeight = 240;

    std::string modules = "101";
    const std::string_view parity = kParity[static_cast<std::size_t>(kBarcode.front() - '0')];
    for (std::size_t index = 1; index < 7; ++index) {
        const std::size_t digit = static_cast<std::size_t>(kBarcode[index] - '0');
        modules += parity[index - 1] == 'L' ? kLeft[digit] : kEven[digit];
    }
    modules += "01010";
    for (std::size_t index = 7; index < kBarcode.size(); ++index) {
        modules += kRight[static_cast<std::size_t>(kBarcode[index] - '0')];
    }
    modules += "101";

    cv::Mat frame(600, 900, CV_8UC3, cv::Scalar(180, 180, 180));
    const int barcode_width = static_cast<int>(modules.size() + 2 * kQuietModules) * kModuleWidth;
    const int left = (frame.cols - barcode_width) / 2;
    const int top = (frame.rows - kBarcodeHeight) / 2;
    cv::rectangle(frame, cv::Rect(left, top, barcode_width, kBarcodeHeight), cv::Scalar(255, 255, 255), cv::FILLED);
    for (std::size_t index = 0; index < modules.size(); ++index) {
        if (modules[index] == '1') {
            cv::rectangle(frame,
                          cv::Rect(left + (kQuietModules + static_cast<int>(index)) * kModuleWidth, top + 20,
                                   kModuleWidth, kBarcodeHeight - 50),
                          cv::Scalar(0, 0, 0), cv::FILLED);
        }
    }
    cv::GaussianBlur(frame, frame, cv::Size(3, 3), 0.7);
    return frame;
}

void TestBarcodeDetectionDoesNotRequireBox() {
    vision::DetectionModule detection;
    const vision::DetectionResult result = detection.Process(MakeBarcodeOnlyFrame());

    Require(!result.box.has_value());
    Require(result.barcodes.size() == 1);
    Require(result.barcodes.front().value == "5901234123457");
}

}  // namespace

int main() {
    TestRotatedRectangleMustRemainInsideFrame();
    TestBarcodeDetectionDoesNotRequireBox();
    return 0;
}
