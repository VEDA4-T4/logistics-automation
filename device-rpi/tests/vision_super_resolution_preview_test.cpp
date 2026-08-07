#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

#include "detection.hpp"

namespace {

namespace vision = logistics::vision;

void Require(const bool condition) {
    if (!condition) {
        throw std::runtime_error("vision super-resolution preview test condition failed");
    }
}

void TestBicubicPreviewUsesProductionSuperResolutionPath() {
    vision::VisionProcessingConfig config;
    config.super_resolution_enabled = true;
    config.super_resolution_backend = vision::SuperResolutionBackend::kBicubic;
    config.super_resolution_scale = 2;
    vision::DetectionModule detector(config);

    cv::Mat source(8, 8, CV_8UC1, cv::Scalar(0));
    source(cv::Rect{ 3, 0, 5, 8 }).setTo(cv::Scalar(255));
    const cv::Mat super_resolved = detector.SuperResolveForPreview(source);
    Require(super_resolved.size() == cv::Size(16, 16));

    cv::Mat nearest;
    cv::resize(source, nearest, super_resolved.size(), 0.0, 0.0, cv::INTER_NEAREST);
    Require(cv::norm(super_resolved, nearest, cv::NORM_L1) > 0.0);
}

}  // namespace

int main() {
    TestBicubicPreviewUsesProductionSuperResolutionPath();
    return 0;
}
