#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "detection.hpp"

namespace {

namespace vision = logistics::vision;

struct DatasetSample final {
    std::filesystem::path image_path;
    std::string expected_barcode;
    cv::Mat image;
};

struct BenchmarkProfile final {
    std::string name;
    vision::VisionProcessingConfig config;
};

struct BenchmarkResult final {
    std::size_t samples{};
    std::size_t boxes{};
    std::size_t barcode_regions{};
    std::size_t decoded{};
    std::size_t correct{};
    std::size_t super_resolution_failures{};
    double elapsed_ms{};
    double wall_ms{};
    double cpu_ms{};
    std::vector<double> sample_latencies_ms;
    std::vector<double> rss_samples_kb;
    vision::DetectionDiagnostics diagnostics;
};

struct Arguments final {
    std::filesystem::path dataset_directory;
    std::filesystem::path manifest_path;
    std::filesystem::path output_path;
    std::filesystem::path fsrcnn_model_path;
    std::filesystem::path visual_output_directory;
    int iterations{ 1 };
    int warmup_iterations{ 1 };
    int duration_seconds{};
    int visual_limit{ 10 };
    std::optional<std::string> profile;
};

std::string_view Trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

int ParsePositiveInteger(const std::string_view value) {
    int parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed <= 0) {
        throw std::invalid_argument("iterations must be a positive integer");
    }
    return parsed;
}

int ParseNonNegativeInteger(const std::string_view value) {
    int parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < 0) {
        throw std::invalid_argument("value must be a non-negative integer");
    }
    return parsed;
}

Arguments ParseArguments(const int argc, char* argv[]) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--help" || option == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " --dataset DIR --manifest FILE [--output FILE] [--iterations N] [--fsrcnn-model FILE]"
                         " [--warmup N] [--duration-seconds N] [--profile NAME]"
                         " [--visual-output DIR] [--visual-limit N]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + std::string(option));
        }
        const std::string value = argv[++index];
        if (option == "--dataset") {
            arguments.dataset_directory = value;
        } else if (option == "--manifest") {
            arguments.manifest_path = value;
        } else if (option == "--output") {
            arguments.output_path = value;
        } else if (option == "--iterations") {
            arguments.iterations = ParsePositiveInteger(value);
        } else if (option == "--warmup") {
            arguments.warmup_iterations = ParseNonNegativeInteger(value);
        } else if (option == "--duration-seconds") {
            arguments.duration_seconds = ParseNonNegativeInteger(value);
        } else if (option == "--profile") {
            arguments.profile = value;
        } else if (option == "--fsrcnn-model") {
            arguments.fsrcnn_model_path = value;
        } else if (option == "--visual-output") {
            arguments.visual_output_directory = value;
        } else if (option == "--visual-limit") {
            arguments.visual_limit = ParsePositiveInteger(value);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(option));
        }
    }
    if (arguments.dataset_directory.empty() || arguments.manifest_path.empty()) {
        throw std::invalid_argument("--dataset and --manifest are required");
    }
    return arguments;
}

std::vector<DatasetSample> LoadDataset(const Arguments& arguments) {
    std::ifstream manifest(arguments.manifest_path);
    if (!manifest) {
        throw std::runtime_error("unable to open manifest: " + arguments.manifest_path.string());
    }

    std::vector<DatasetSample> samples;
    std::string line;
    std::size_t line_number{};
    while (std::getline(manifest, line)) {
        ++line_number;
        if (line_number == 1 && line.starts_with("filename,")) {
            continue;
        }
        const auto delimiter = line.find(',');
        if (delimiter == std::string::npos) {
            throw std::runtime_error("invalid manifest row " + std::to_string(line_number));
        }
        const std::string_view filename = Trim(std::string_view(line).substr(0, delimiter));
        const std::string_view barcode = Trim(std::string_view(line).substr(delimiter + 1));
        if (filename.empty() || barcode.empty()) {
            throw std::runtime_error("empty manifest value on row " + std::to_string(line_number));
        }
        DatasetSample sample{
            .image_path = arguments.dataset_directory / filename,
            .expected_barcode = std::string(barcode),
        };
        sample.image = cv::imread(sample.image_path.string(), cv::IMREAD_COLOR);
        if (sample.image.empty()) {
            throw std::runtime_error("unable to read image: " + sample.image_path.string());
        }
        samples.push_back(std::move(sample));
    }
    if (samples.empty()) {
        throw std::runtime_error("manifest contains no samples");
    }
    return samples;
}

std::vector<BenchmarkProfile> MakeProfiles(const std::filesystem::path& fsrcnn_model_path) {
    vision::VisionProcessingConfig baseline;
    baseline.perspective_rectification = false;
    baseline.contrast_enhancement = false;
    baseline.super_resolution_enabled = false;
    baseline.failure_frames_before_super_resolution = 1;

    auto rectification = baseline;
    rectification.perspective_rectification = true;

    auto contrast = rectification;
    contrast.contrast_enhancement = true;

    auto bicubic = contrast;
    bicubic.super_resolution_enabled = true;
    bicubic.super_resolution_backend = vision::SuperResolutionBackend::kBicubic;

    auto detection_sr = baseline;
    detection_sr.super_resolution_enabled = true;
    detection_sr.barcode_detection_fallback = true;
    detection_sr.barcode_decode_fallback = false;

    auto decode_sr = contrast;
    decode_sr.super_resolution_enabled = true;
    decode_sr.barcode_detection_fallback = false;
    decode_sr.barcode_decode_fallback = true;

    std::vector<BenchmarkProfile> profiles{
        { .name = "baseline", .config = baseline },
        { .name = "rectification", .config = rectification },
        { .name = "rectification_contrast", .config = contrast },
        { .name = "barcode_detection_bicubic_sr_x2", .config = detection_sr },
        { .name = "barcode_decode_bicubic_sr_x2", .config = decode_sr },
        { .name = "full_bicubic_sr_x2", .config = bicubic },
    };
    if (!fsrcnn_model_path.empty()) {
        auto fsrcnn = bicubic;
        fsrcnn.super_resolution_backend = vision::SuperResolutionBackend::kFsrcnn;
        fsrcnn.super_resolution_model_path = fsrcnn_model_path;
        profiles.push_back({ .name = "full_fsrcnn_sr_x2", .config = std::move(fsrcnn) });
    }
    return profiles;
}

std::vector<BenchmarkProfile> SelectProfiles(std::vector<BenchmarkProfile> profiles,
                                             const std::optional<std::string>& selected_profile) {
    if (!selected_profile.has_value()) {
        return profiles;
    }
    const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const BenchmarkProfile& candidate) {
        return candidate.name == *selected_profile;
    });
    if (profile == profiles.end()) {
        throw std::invalid_argument("unknown profile: " + *selected_profile);
    }
    return { *profile };
}

void AddDiagnostics(vision::DetectionDiagnostics& target, const vision::DetectionDiagnostics& source) {
    target.box_detection_ms += source.box_detection_ms;
    target.barcode_detection_ms += source.barcode_detection_ms;
    target.barcode_decode_ms += source.barcode_decode_ms;
    target.perspective_rectification_ms += source.perspective_rectification_ms;
    target.contrast_enhancement_ms += source.contrast_enhancement_ms;
    target.super_resolution_ms += source.super_resolution_ms;
}

double CurrentResidentSetKb() {
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    long total_pages{};
    long resident_pages{};
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size > 0 && statm >> total_pages >> resident_pages) {
        return static_cast<double>(resident_pages) * static_cast<double>(page_size) / 1024.0;
    }
#endif
    return 0.0;
}

BenchmarkResult RunProfile(const BenchmarkProfile& profile, const std::vector<DatasetSample>& samples,
                           const Arguments& arguments) {
    vision::DetectionModule detector(profile.config);
    for (int iteration = 0; iteration < arguments.warmup_iterations; ++iteration) {
        for (const DatasetSample& sample : samples) {
            static_cast<void>(detector.Process(sample.image, true));
        }
    }

    BenchmarkResult benchmark;
    benchmark.rss_samples_kb.push_back(CurrentResidentSetKb());
    const auto benchmark_started = std::chrono::steady_clock::now();
    const std::clock_t cpu_started = std::clock();
    int iteration{};
    do {
        for (const DatasetSample& sample : samples) {
            const auto started = std::chrono::steady_clock::now();
            const vision::DetectionResult result = detector.Process(sample.image, true);
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            benchmark.elapsed_ms += elapsed_ms;
            benchmark.sample_latencies_ms.push_back(elapsed_ms);
            ++benchmark.samples;
            benchmark.boxes += result.box.has_value() ? 1U : 0U;
            benchmark.barcode_regions += result.diagnostics.barcode_region_detected ? 1U : 0U;
            benchmark.decoded += result.diagnostics.barcode_decoded ? 1U : 0U;
            benchmark.super_resolution_failures += result.diagnostics.super_resolution_failed ? 1U : 0U;
            if (std::any_of(
                    result.barcodes.begin(), result.barcodes.end(),
                    [&](const vision::DetectedBarcode& barcode) { return barcode.value == sample.expected_barcode; })) {
                ++benchmark.correct;
            }
            AddDiagnostics(benchmark.diagnostics, result.diagnostics);
        }
        benchmark.rss_samples_kb.push_back(CurrentResidentSetKb());
        ++iteration;
    } while (iteration < arguments.iterations ||
             (arguments.duration_seconds > 0 &&
              std::chrono::steady_clock::now() - benchmark_started < std::chrono::seconds(arguments.duration_seconds)));
    benchmark.wall_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - benchmark_started).count();
    const std::clock_t cpu_finished = std::clock();
    if (cpu_started != static_cast<std::clock_t>(-1) && cpu_finished != static_cast<std::clock_t>(-1)) {
        benchmark.cpu_ms =
            static_cast<double>(cpu_finished - cpu_started) * 1000.0 / static_cast<double>(CLOCKS_PER_SEC);
    }
    return benchmark;
}

double Percentage(const std::size_t value, const std::size_t total) {
    return total == 0 ? 0.0 : static_cast<double>(value) * 100.0 / static_cast<double>(total);
}

double Average(const double value, const std::size_t count) {
    return count == 0 ? 0.0 : value / static_cast<double>(count);
}

double Percentile(std::vector<double> values, const double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(percentile * static_cast<double>(values.size())));
    const std::size_t index = std::clamp(rank, std::size_t{ 1 }, values.size()) - 1;
    return values[index];
}

double AverageRange(const std::vector<double>& values, const std::size_t begin, const std::size_t end) {
    if (begin >= end || end > values.size()) {
        return 0.0;
    }
    double sum{};
    for (std::size_t index = begin; index < end; ++index) {
        sum += values[index];
    }
    return sum / static_cast<double>(end - begin);
}

double ThroughputRange(const std::vector<double>& latencies, const std::size_t begin, const std::size_t end) {
    const double average_ms = AverageRange(latencies, begin, end);
    return average_ms <= 0.0 ? 0.0 : 1000.0 / average_ms;
}

double ChangePercent(const double first, const double last) {
    return first <= 0.0 ? 0.0 : (last - first) * 100.0 / first;
}

void WriteHeader(std::ostream& output) {
    output << "profile,samples,box_rate_percent,barcode_region_rate_percent,decode_rate_percent,"
              "accuracy_percent,sr_failure_rate_percent,average_total_ms,p50_total_ms,p95_total_ms,p99_total_ms,"
              "throughput_fps,cpu_percent,average_rss_kb,peak_rss_kb,first_half_fps,last_half_fps,"
              "throughput_change_percent,rss_growth_kb,box_ms,barcode_detection_ms,rectification_ms,contrast_ms,"
              "super_resolution_ms,barcode_decode_ms\n";
}

void WriteResult(std::ostream& output, const std::string_view name, const BenchmarkResult& result) {
    const std::size_t latency_midpoint = result.sample_latencies_ms.size() / 2;
    const std::size_t rss_midpoint = result.rss_samples_kb.size() / 2;
    const double first_half_fps = ThroughputRange(result.sample_latencies_ms, 0, latency_midpoint);
    const double last_half_fps =
        ThroughputRange(result.sample_latencies_ms, latency_midpoint, result.sample_latencies_ms.size());
    const double first_half_rss = AverageRange(result.rss_samples_kb, 0, rss_midpoint);
    const double last_half_rss = AverageRange(result.rss_samples_kb, rss_midpoint, result.rss_samples_kb.size());
    const double peak_rss = result.rss_samples_kb.empty()
                                ? 0.0
                                : *std::max_element(result.rss_samples_kb.begin(), result.rss_samples_kb.end());
    output << name << ',' << result.samples << ',' << Percentage(result.boxes, result.samples) << ','
           << Percentage(result.barcode_regions, result.samples) << ',' << Percentage(result.decoded, result.samples)
           << ',' << Percentage(result.correct, result.samples) << ','
           << Percentage(result.super_resolution_failures, result.samples) << ','
           << Average(result.elapsed_ms, result.samples) << ',' << Percentile(result.sample_latencies_ms, 0.50) << ','
           << Percentile(result.sample_latencies_ms, 0.95) << ',' << Percentile(result.sample_latencies_ms, 0.99) << ','
           << (result.wall_ms <= 0.0 ? 0.0 : static_cast<double>(result.samples) * 1000.0 / result.wall_ms) << ','
           << (result.wall_ms <= 0.0 ? 0.0 : result.cpu_ms * 100.0 / result.wall_ms) << ','
           << AverageRange(result.rss_samples_kb, 0, result.rss_samples_kb.size()) << ',' << peak_rss << ','
           << first_half_fps << ',' << last_half_fps << ',' << ChangePercent(first_half_fps, last_half_fps) << ','
           << (last_half_rss - first_half_rss) << ',' << Average(result.diagnostics.box_detection_ms, result.samples)
           << ',' << Average(result.diagnostics.barcode_detection_ms, result.samples) << ','
           << Average(result.diagnostics.perspective_rectification_ms, result.samples) << ','
           << Average(result.diagnostics.contrast_enhancement_ms, result.samples) << ','
           << Average(result.diagnostics.super_resolution_ms, result.samples) << ','
           << Average(result.diagnostics.barcode_decode_ms, result.samples) << '\n';
}

cv::Rect ExpandAndClip(const cv::Rect& rectangle, const cv::Size frame_size, const int padding) {
    const cv::Rect expanded{ rectangle.x - padding, rectangle.y - padding, rectangle.width + padding * 2,
                             rectangle.height + padding * 2 };
    return expanded & cv::Rect{ 0, 0, frame_size.width, frame_size.height };
}

cv::Mat SelectPreviewRegion(const DatasetSample& sample, const vision::DetectionResult& result) {
    if (!result.barcodes.empty() && !result.barcodes.front().corners.empty()) {
        const cv::Rect barcode_bounds = cv::boundingRect(result.barcodes.front().corners);
        const cv::Rect preview_bounds = ExpandAndClip(barcode_bounds, sample.image.size(), 16);
        if (!preview_bounds.empty()) {
            return sample.image(preview_bounds).clone();
        }
    }
    if (result.box.has_value() && !result.box->roi.empty()) {
        return sample.image(result.box->roi).clone();
    }

    constexpr int kFallbackPreviewSize = 512;
    const int width = std::min(sample.image.cols, kFallbackPreviewSize);
    const int height = std::min(sample.image.rows, kFallbackPreviewSize);
    const int x = (sample.image.cols - width) / 2;
    const int y = (sample.image.rows - height) / 2;
    return sample.image(cv::Rect{ x, y, width, height }).clone();
}

cv::Mat AddPanelLabel(const cv::Mat& image, const std::string_view label) {
    constexpr int kHeaderHeight = 40;
    cv::Mat bgr;
    if (image.channels() == 1) {
        cv::cvtColor(image, bgr, cv::COLOR_GRAY2BGR);
    } else {
        bgr = image;
    }
    cv::Mat panel(bgr.rows + kHeaderHeight, bgr.cols, CV_8UC3, cv::Scalar(24, 24, 24));
    bgr.copyTo(panel(cv::Rect{ 0, kHeaderHeight, bgr.cols, bgr.rows }));
    cv::putText(panel, std::string(label), cv::Point{ 12, 27 }, cv::FONT_HERSHEY_SIMPLEX, 0.65,
                cv::Scalar(245, 245, 245), 1, cv::LINE_AA);
    return panel;
}

vision::VisionProcessingConfig PreviewConfig(const vision::SuperResolutionBackend backend,
                                             const std::filesystem::path& model_path = {}) {
    vision::VisionProcessingConfig config;
    config.super_resolution_enabled = true;
    config.super_resolution_backend = backend;
    config.super_resolution_scale = 2;
    config.super_resolution_model_path = model_path;
    return config;
}

void WriteVisualComparisons(const Arguments& arguments, const std::vector<DatasetSample>& samples) {
    if (arguments.visual_output_directory.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(arguments.visual_output_directory, error);
    if (error) {
        throw std::runtime_error("unable to create visual output directory: " + error.message());
    }

    vision::VisionProcessingConfig locator_config;
    locator_config.super_resolution_enabled = false;
    vision::DetectionModule locator(locator_config);
    vision::DetectionModule bicubic(PreviewConfig(vision::SuperResolutionBackend::kBicubic));
    std::optional<vision::DetectionModule> fsrcnn;
    if (!arguments.fsrcnn_model_path.empty()) {
        fsrcnn.emplace(PreviewConfig(vision::SuperResolutionBackend::kFsrcnn, arguments.fsrcnn_model_path));
    }

    const std::size_t count = std::min(samples.size(), static_cast<std::size_t>(arguments.visual_limit));
    for (std::size_t index = 0; index < count; ++index) {
        const DatasetSample& sample = samples[index];
        const vision::DetectionResult located = locator.Process(sample.image, false);
        const cv::Mat region = SelectPreviewRegion(sample, located);

        cv::Mat nearest;
        cv::resize(region, nearest, cv::Size{}, 2.0, 2.0, cv::INTER_NEAREST);
        std::vector<cv::Mat> panels{
            AddPanelLabel(nearest, "ORIGINAL x2 (NEAREST)"),
            AddPanelLabel(bicubic.SuperResolveForPreview(region), "BICUBIC SR x2"),
        };
        if (fsrcnn.has_value()) {
            panels.push_back(AddPanelLabel(fsrcnn->SuperResolveForPreview(region), "FSRCNN SR x2"));
        }
        cv::Mat comparison;
        cv::hconcat(panels, comparison);

        std::ostringstream filename;
        filename << std::setw(3) << std::setfill('0') << index + 1 << '-' << sample.image_path.stem().string()
                 << "-sr-comparison.png";
        const std::filesystem::path output_path = arguments.visual_output_directory / filename.str();
        if (!cv::imwrite(output_path.string(), comparison)) {
            throw std::runtime_error("unable to write visual comparison: " + output_path.string());
        }
    }
    std::clog << "[vision-benchmark] wrote " << count << " SR visual comparison image(s) to "
              << arguments.visual_output_directory << '\n';
}

}  // namespace

int main(const int argc, char* argv[]) {
    try {
        const Arguments arguments = ParseArguments(argc, argv);
        const std::vector<DatasetSample> samples = LoadDataset(arguments);
        const std::vector<BenchmarkProfile> profiles =
            SelectProfiles(MakeProfiles(arguments.fsrcnn_model_path), arguments.profile);

        std::ostringstream results;
        results << std::fixed << std::setprecision(3);
        WriteHeader(results);
        for (const BenchmarkProfile& profile : profiles) {
            WriteResult(results, profile.name, RunProfile(profile, samples, arguments));
        }
        std::cout << results.str();
        if (!arguments.output_path.empty()) {
            std::ofstream output(arguments.output_path);
            if (!output) {
                throw std::runtime_error("unable to open output: " + arguments.output_path.string());
            }
            output << results.str();
        }
        WriteVisualComparisons(arguments, samples);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[vision-benchmark][ERROR] " << error.what() << '\n';
        return 2;
    }
}
