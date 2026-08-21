#include "vision_processing_config.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace vision = logistics::vision;

std::filesystem::path TemporaryConfigPath() {
    return std::filesystem::temp_directory_path() /
           ("vision-processing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".ini");
}

void TestDefaultsWhenSectionIsMissing() {
    const auto path = TemporaryConfigPath();
    {
        std::ofstream output(path);
        assert(output);
        output << "[device]\ndevice_id=PI-VISION-01\n";
    }
    const auto config = vision::LoadVisionProcessingConfig(path);
    assert(config.perspective_rectification);
    assert(config.contrast_enhancement);
    assert(config.super_resolution_enabled);
    assert(config.super_resolution_backend == vision::SuperResolutionBackend::kBicubic);
    assert(config.super_resolution_scale == 2);
    assert(config.failure_frame_capture.enabled);
    assert(config.failure_frame_capture.directory == "/tmp/logistics-vision-failures");
    assert(config.failure_frame_capture.maximum_frames == 200);
    std::filesystem::remove(path);
}

void TestConfiguredFsrcnnModelPathIsRelativeToConfig() {
    const auto path = TemporaryConfigPath();
    {
        std::ofstream output(path);
        assert(output);
        output << R"ini(
[vision_processing]
perspective_rectification=false
contrast_enhancement=false
super_resolution_enabled=true
barcode_detection_fallback=false
barcode_decode_fallback=true
super_resolution_backend=fsrcnn
super_resolution_scale=3
failure_frames_before_super_resolution=4
maximum_super_resolution_input_pixels=123456
super_resolution_model_path=models/FSRCNN_x3.pb
failure_frame_capture_enabled=true
failure_frame_directory=failed-frames
maximum_failure_frames=25
failure_frame_jpeg_quality=85
)ini";
    }
    const auto config = vision::LoadVisionProcessingConfig(path);
    assert(!config.perspective_rectification);
    assert(!config.contrast_enhancement);
    assert(!config.barcode_detection_fallback);
    assert(config.super_resolution_backend == vision::SuperResolutionBackend::kFsrcnn);
    assert(config.super_resolution_scale == 3);
    assert(config.failure_frames_before_super_resolution == 4);
    assert(config.maximum_super_resolution_input_pixels == 123456);
    assert(config.super_resolution_model_path == path.parent_path() / "models/FSRCNN_x3.pb");
    assert(config.failure_frame_capture.directory == path.parent_path() / "failed-frames");
    assert(config.failure_frame_capture.maximum_frames == 25);
    assert(config.failure_frame_capture.jpeg_quality == 85);
    std::filesystem::remove(path);
}

void TestInvalidBackendIsRejected() {
    const auto path = TemporaryConfigPath();
    {
        std::ofstream output(path);
        assert(output);
        output << "[vision_processing]\nsuper_resolution_backend=gan\n";
    }
    bool rejected = false;
    try {
        static_cast<void>(vision::LoadVisionProcessingConfig(path));
    } catch (const vision::VisionProcessingConfigError&) {
        rejected = true;
    }
    assert(rejected);
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    TestDefaultsWhenSectionIsMissing();
    TestConfiguredFsrcnnModelPathIsRelativeToConfig();
    TestInvalidBackendIsRejected();
    return 0;
}
