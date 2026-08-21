#pragma once

#include <cstddef>
#include <filesystem>
#include <stdexcept>

namespace logistics::vision {

enum class SuperResolutionBackend {
    kBicubic,
    kFsrcnn,
};

struct FailureFrameCaptureConfig final {
    bool enabled{ true };
    std::filesystem::path directory{ "/tmp/logistics-vision-failures" };
    std::size_t maximum_frames{ 200 };
    int jpeg_quality{ 90 };

    [[nodiscard]] bool IsValid() const noexcept;
};

struct VisionProcessingConfig final {
    bool perspective_rectification{ true };
    bool contrast_enhancement{ true };
    bool super_resolution_enabled{ true };
    bool barcode_detection_fallback{ true };
    bool barcode_decode_fallback{ true };
    SuperResolutionBackend super_resolution_backend{ SuperResolutionBackend::kBicubic };
    int super_resolution_scale{ 2 };
    int failure_frames_before_super_resolution{ 2 };
    std::size_t maximum_super_resolution_input_pixels{ 300000 };
    int preassignment_timeout_ms{ 3000 };
    int barcode_timeout_ms{ 10000 };
    std::filesystem::path super_resolution_model_path;
    FailureFrameCaptureConfig failure_frame_capture;

    [[nodiscard]] bool IsValid() const noexcept;
};

class VisionProcessingConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] VisionProcessingConfig LoadVisionProcessingConfig(const std::filesystem::path& path);

}  // namespace logistics::vision
