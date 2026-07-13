#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>

#include <iostream>
#include <string>

namespace {

    constexpr int kEscapeKey = 27;
    constexpr int kWaitKeyDelayMs = 1;

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

    std::cout << "Camera started. Press q or Esc to exit.\n";

    cv::Mat frame;
    while (true) {
        if (!camera.read(frame) || frame.empty()) {
            std::cerr << "Failed to receive a camera frame.\n";
            return 1;
        }

        cv::imshow(kWindowName, frame);

        const int key = cv::waitKey(kWaitKeyDelayMs) & 0xff;
        if (key == 'q' || key == kEscapeKey) {
            break;
        }
    }

    return 0;
}
