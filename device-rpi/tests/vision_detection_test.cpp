#include <stdexcept>

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

}  // namespace

int main() {
    TestRotatedRectangleMustRemainInsideFrame();
    return 0;
}
