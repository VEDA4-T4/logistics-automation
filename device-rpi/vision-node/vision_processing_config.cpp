#include "vision_processing_config.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace logistics::vision {
namespace {

std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

[[noreturn]] void ThrowLineError(const std::filesystem::path& path, const std::size_t line_number,
                                 const std::string_view detail) {
    throw VisionProcessingConfigError(path.string() + ":" + std::to_string(line_number) + ": " + std::string(detail));
}

bool ParseBoolean(const std::filesystem::path& path, const std::size_t line_number, const std::string_view key,
                  const std::string_view value) {
    std::string normalized(value);
    for (char& character : normalized) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (normalized == "true" || normalized == "yes" || normalized == "1") {
        return true;
    }
    if (normalized == "false" || normalized == "no" || normalized == "0") {
        return false;
    }
    ThrowLineError(path, line_number, std::string(key) + " must be true or false");
}

template <typename Integer>
Integer ParseInteger(const std::filesystem::path& path, const std::size_t line_number, const std::string_view key,
                     const std::string_view value, const Integer minimum, const Integer maximum) {
    Integer parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < minimum || parsed > maximum) {
        ThrowLineError(path, line_number, std::string(key) + " is outside the allowed range");
    }
    return parsed;
}

void AssignValue(VisionProcessingConfig& config, const std::filesystem::path& path, const std::size_t line_number,
                 const std::string_view key, const std::string_view value) {
    if (key == "perspective_rectification") {
        config.perspective_rectification = ParseBoolean(path, line_number, key, value);
    } else if (key == "contrast_enhancement") {
        config.contrast_enhancement = ParseBoolean(path, line_number, key, value);
    } else if (key == "super_resolution_enabled") {
        config.super_resolution_enabled = ParseBoolean(path, line_number, key, value);
    } else if (key == "barcode_detection_fallback") {
        config.barcode_detection_fallback = ParseBoolean(path, line_number, key, value);
    } else if (key == "barcode_decode_fallback") {
        config.barcode_decode_fallback = ParseBoolean(path, line_number, key, value);
    } else if (key == "super_resolution_backend") {
        if (value == "bicubic") {
            config.super_resolution_backend = SuperResolutionBackend::kBicubic;
        } else if (value == "fsrcnn") {
            config.super_resolution_backend = SuperResolutionBackend::kFsrcnn;
        } else {
            ThrowLineError(path, line_number, "super_resolution_backend must be bicubic or fsrcnn");
        }
    } else if (key == "super_resolution_scale") {
        config.super_resolution_scale = ParseInteger<int>(path, line_number, key, value, 2, 4);
    } else if (key == "failure_frames_before_super_resolution") {
        config.failure_frames_before_super_resolution =
            ParseInteger<int>(path, line_number, key, value, 1, std::numeric_limits<int>::max());
    } else if (key == "maximum_super_resolution_input_pixels") {
        config.maximum_super_resolution_input_pixels =
            ParseInteger<std::size_t>(path, line_number, key, value, 1, std::numeric_limits<std::size_t>::max());
    } else if (key == "super_resolution_model_path") {
        config.super_resolution_model_path = std::string(value);
    } else if (key == "failure_frame_capture_enabled") {
        config.failure_frame_capture.enabled = ParseBoolean(path, line_number, key, value);
    } else if (key == "failure_frame_directory") {
        config.failure_frame_capture.directory = std::string(value);
    } else if (key == "maximum_failure_frames") {
        config.failure_frame_capture.maximum_frames =
            ParseInteger<std::size_t>(path, line_number, key, value, 1, 100000);
    } else if (key == "failure_frame_jpeg_quality") {
        config.failure_frame_capture.jpeg_quality = ParseInteger<int>(path, line_number, key, value, 1, 100);
    } else {
        ThrowLineError(path, line_number, "unknown [vision_processing] setting: " + std::string(key));
    }
}

}  // namespace

bool FailureFrameCaptureConfig::IsValid() const noexcept {
    return !enabled || (!directory.empty() && maximum_frames > 0 && jpeg_quality >= 1 && jpeg_quality <= 100);
}

bool VisionProcessingConfig::IsValid() const noexcept {
    const bool valid_backend = !super_resolution_enabled ||
                               super_resolution_backend != SuperResolutionBackend::kFsrcnn ||
                               !super_resolution_model_path.empty();
    return super_resolution_scale >= 2 && super_resolution_scale <= 4 && failure_frames_before_super_resolution > 0 &&
           maximum_super_resolution_input_pixels > 0 && valid_backend && failure_frame_capture.IsValid();
}

VisionProcessingConfig LoadVisionProcessingConfig(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw VisionProcessingConfigError("unable to open vision configuration: " + path.string());
    }

    VisionProcessingConfig config;
    std::string section;
    std::string line;
    std::size_t line_number{};
    std::unordered_set<std::string> assigned_keys;
    while (std::getline(input, line)) {
        ++line_number;
        std::string_view text = Trim(line);
        if (line_number == 1 && text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
            text = Trim(text);
        }
        if (text.empty() || text.front() == '#' || text.front() == ';') {
            continue;
        }
        if (text.front() == '[' && text.back() == ']') {
            section = std::string(Trim(text.substr(1, text.size() - 2)));
            continue;
        }
        if (section != "vision_processing") {
            continue;
        }

        const auto delimiter = text.find('=');
        if (delimiter == std::string_view::npos) {
            ThrowLineError(path, line_number, "expected key=value");
        }
        const std::string_view key = Trim(text.substr(0, delimiter));
        const std::string_view value = Trim(text.substr(delimiter + 1));
        if (key.empty() || !assigned_keys.emplace(key).second) {
            ThrowLineError(path, line_number, "empty or duplicate [vision_processing] setting");
        }
        AssignValue(config, path, line_number, key, value);
    }

    if (!config.super_resolution_model_path.empty() && config.super_resolution_model_path.is_relative()) {
        config.super_resolution_model_path = path.parent_path() / config.super_resolution_model_path;
    }
    if (assigned_keys.contains("failure_frame_directory") && !config.failure_frame_capture.directory.empty() &&
        config.failure_frame_capture.directory.is_relative()) {
        config.failure_frame_capture.directory = path.parent_path() / config.failure_frame_capture.directory;
    }
    if (!config.IsValid()) {
        throw VisionProcessingConfigError("invalid [vision_processing] configuration in " + path.string());
    }
    return config;
}

}  // namespace logistics::vision
