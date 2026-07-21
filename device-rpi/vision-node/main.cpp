#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

#include "detection.hpp"
#include "vision_mqtt_workflow.hpp"

#ifdef LOGISTICS_VISION_MQTT_ENABLED
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/device_status.hpp"
#include "logistics/device/image_uploader.hpp"
#include "logistics/device/mqtt_node_client.hpp"
#include "logistics/device/mqtt_node_config.hpp"
#include "logistics/device/mqtt_time.hpp"
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kEscapeKey = 27;
constexpr int kWaitKeyDelayMs = 1;
constexpr int kReconnectIntervalMs = 3000;
constexpr int kMaximumConsecutiveFrameErrors = 3;
constexpr int kReconnectPollIntervalMs = 100;
constexpr int kMaximumCameraDimension = 8192;
constexpr int kMaximumCameraFps = 240;
constexpr std::size_t kBarcodeCornerCount = 4;
constexpr double kLatencySmoothingFactor = 0.1;
const cv::Scalar kBoxOutlineColor{ 255, 128, 0 };
const cv::Scalar kBarcodeBoxColor{ 0, 255, 0 };
const cv::Scalar kErrorColor{ 0, 0, 255 };
const cv::Scalar kTextColor{ 255, 255, 255 };

struct CameraSettings {
    int width = 1280;
    int height = 720;
    int fps = 30;
    std::string config_path = "device-rpi/config/node.ini";
};

enum class ParseStatus {
    kSuccess,
    kHelp,
    kError,
};

struct LatencyMetrics {
    double capture_ms = 0.0;
    double processing_ms = 0.0;
    double total_ms = 0.0;
};

class LatencyTracker final {
public:
    void Update(const LatencyMetrics& current) {
        if (!initialized_) {
            average_ = current;
            initialized_ = true;
            return;
        }

        average_.capture_ms = Smooth(average_.capture_ms, current.capture_ms);
        average_.processing_ms = Smooth(average_.processing_ms, current.processing_ms);
        average_.total_ms = Smooth(average_.total_ms, current.total_ms);
    }

    [[nodiscard]] const LatencyMetrics& average() const {
        return average_;
    }

private:
    [[nodiscard]] static double Smooth(const double previous, const double current) {
        return previous * (1.0 - kLatencySmoothingFactor) + current * kLatencySmoothingFactor;
    }

    bool initialized_ = false;
    LatencyMetrics average_;
};

void PrintUsage(const char* executable) {
    std::cout << "Usage: " << executable << " [--width N] [--height N] [--fps N] [--config node.ini]\n"
              << "Defaults: --width 1280 --height 720 --fps 30 --config device-rpi/config/node.ini\n";
}

bool ParsePositiveInteger(const std::string_view value, int& output) {
    int parsed_value = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
    if (error != std::errc{} || end != value.data() + value.size() || parsed_value <= 0) {
        return false;
    }

    output = parsed_value;
    return true;
}

ParseStatus ParseArguments(const int argc, char* argv[], CameraSettings& settings) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return ParseStatus::kHelp;
        }

        if (argument == "--config") {
            if (index + 1 >= argc || std::string_view(argv[index + 1]).empty()) {
                std::cerr << "Missing value for --config\n";
                return ParseStatus::kError;
            }
            settings.config_path = argv[++index];
            continue;
        }

        if (argument != "--width" && argument != "--height" && argument != "--fps") {
            std::cerr << "Unknown option: " << argument << '\n';
            PrintUsage(argv[0]);
            return ParseStatus::kError;
        }

        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return ParseStatus::kError;
        }

        int value = 0;
        if (!ParsePositiveInteger(argv[++index], value)) {
            std::cerr << "Invalid positive integer for " << argument << ": " << argv[index] << '\n';
            return ParseStatus::kError;
        }

        if (argument == "--width") {
            settings.width = value;
        } else if (argument == "--height") {
            settings.height = value;
        } else {
            settings.fps = value;
        }
    }

    if (settings.width > kMaximumCameraDimension || settings.height > kMaximumCameraDimension ||
        settings.fps > kMaximumCameraFps) {
        std::cerr << "Camera settings exceed supported limits: maximum dimension " << kMaximumCameraDimension
                  << ", maximum FPS " << kMaximumCameraFps << ".\n";
        return ParseStatus::kError;
    }

    return ParseStatus::kSuccess;
}

std::string BuildCameraPipeline(const CameraSettings& settings) {
    std::string pipeline = "libcamerasrc ! video/x-raw,width=";
    pipeline += std::to_string(settings.width);
    pipeline += ",height=";
    pipeline += std::to_string(settings.height);
    pipeline += ",framerate=";
    pipeline += std::to_string(settings.fps);
    pipeline += "/1 ! videoconvert ! video/x-raw,format=BGR ! ";
    pipeline += "appsink drop=true max-buffers=1 sync=false wait-on-eos=false";
    return pipeline;
}

bool OpenCamera(cv::VideoCapture& camera, const CameraSettings& settings) {
    camera.release();
    return camera.open(BuildCameraPipeline(settings), cv::CAP_GSTREAMER);
}

bool IsExitKey(const int key) {
    const int normalized_key = key & 0xff;
    return normalized_key == 'q' || normalized_key == kEscapeKey;
}

bool WaitForReconnectOrExit() {
    int elapsed_ms = 0;
    while (elapsed_ms < kReconnectIntervalMs) {
        const int wait_ms = std::min(kReconnectPollIntervalMs, kReconnectIntervalMs - elapsed_ms);
        if (IsExitKey(cv::waitKey(wait_ms))) {
            return true;
        }
        elapsed_ms += wait_ms;
    }
    return false;
}

template <typename Duration>
double ToMilliseconds(const Duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void ShowCameraError(const CameraSettings& settings, const std::string& message, const char* window_name) {
    cv::Mat error_frame(settings.height, settings.width, CV_8UC3, cv::Scalar::all(0));
    cv::putText(error_frame, message, cv::Point(30, 60), cv::FONT_HERSHEY_SIMPLEX, 0.8, kErrorColor, 2);
    cv::putText(error_frame, "Retrying camera connection...", cv::Point(30, 100), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                kTextColor, 2);
    cv::imshow(window_name, error_frame);
}

void DrawDetectionResult(cv::Mat& frame, const logistics::vision::DetectionResult& result) {
    if (!result.box.has_value()) {
        cv::putText(frame, "BOX: not found", cv::Point(20, 35), cv::FONT_HERSHEY_SIMPLEX, 0.8, kErrorColor, 2);
        return;
    }

    cv::Point2f box_corners[kBarcodeCornerCount];
    result.box->outline.points(box_corners);
    for (std::size_t index = 0; index < kBarcodeCornerCount; ++index) {
        cv::line(frame, box_corners[index], box_corners[(index + 1) % kBarcodeCornerCount], kBoxOutlineColor, 2);
    }

    const cv::Point center{ cvRound(result.box->outline.center.x), cvRound(result.box->outline.center.y) };
    const cv::Point box_label_position{ result.box->roi.x, std::max(25, result.box->roi.y - 8) };
    cv::circle(frame, center, 4, kBoxOutlineColor, cv::FILLED);
    cv::putText(frame, "BOX", box_label_position, cv::FONT_HERSHEY_SIMPLEX, 0.7, kBoxOutlineColor, 2);

    for (const logistics::vision::DetectedBarcode& barcode : result.barcodes) {
        if (barcode.corners.size() == kBarcodeCornerCount) {
            for (std::size_t index = 0; index < kBarcodeCornerCount; ++index) {
                cv::line(frame, barcode.corners[index], barcode.corners[(index + 1) % kBarcodeCornerCount],
                         kBarcodeBoxColor, 2);
            }

            cv::putText(frame, barcode.type + ": " + barcode.value, barcode.corners.front(), cv::FONT_HERSHEY_SIMPLEX,
                        0.6, kBarcodeBoxColor, 2);
        }
    }
}

void DrawLatency(cv::Mat& frame, const LatencyMetrics& latency) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "capture " << latency.capture_ms << " ms | detect "
         << latency.processing_ms << " ms | total " << latency.total_ms << " ms";
    cv::putText(frame, text.str(), cv::Point(20, frame.rows - 20), cv::FONT_HERSHEY_SIMPLEX, 0.6, kTextColor, 2);
}

logistics::vision::VisionObservation MakeObservation(const cv::Mat& frame,
                                                     const logistics::vision::DetectionResult& result,
                                                     std::string image_name) {
    const cv::Rect& box = result.box->roi;
    std::optional<std::string> barcode;
    if (!result.barcodes.empty()) {
        barcode = result.barcodes.front().value;
    }
    return {
        .image_name = std::move(image_name),
        .box_x = box.x,
        .box_y = box.y,
        .box_width = box.width,
        .box_height = box.height,
        .frame_width = frame.cols,
        .frame_height = frame.rows,
        .barcode = std::move(barcode),
    };
}

#ifdef LOGISTICS_VISION_MQTT_ENABLED
logistics::contracts::mqtt::MqttMessage MakeVisionError(std::string_view device_id, std::string message_id,
                                                        std::string timestamp, std::string error_code,
                                                        std::string current_state, std::string message,
                                                        std::optional<std::string> work_id = std::nullopt) {
    namespace mqtt = logistics::contracts::mqtt;
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = std::move(message_id),
        .message_type = mqtt::MessageType::kErrorOccurred,
        .source_id = std::string(device_id),
        .timestamp = std::move(timestamp),
        .data =
            mqtt::ErrorOccurredPayload{
                .job_id = std::move(work_id),
                .error_code = std::move(error_code),
                .error_level = "ERROR",
                .current_state = std::move(current_state),
                .message = std::move(message),
                .distance = std::nullopt,
            },
    };
}
#endif

}  // namespace

int main(const int argc, char* argv[]) {
    CameraSettings settings;
    const ParseStatus parse_status = ParseArguments(argc, argv, settings);
    if (parse_status == ParseStatus::kHelp) {
        return 0;
    }
    if (parse_status == ParseStatus::kError) {
        return 2;
    }

#ifdef LOGISTICS_VISION_MQTT_ENABLED
    logistics::device::MqttNodeConfig mqtt_config;
    try {
        mqtt_config = logistics::device::LoadMqttNodeConfig(settings.config_path);
    } catch (const logistics::device::NodeConfigError& error) {
        std::cerr << "[vision][ERROR] " << error.what() << '\n';
        return 2;
    }
    const std::string device_id = mqtt_config.device_id;
    std::unique_ptr<logistics::device::ImageUploader> image_uploader;
    if (mqtt_config.image_upload_enabled) {
        image_uploader = std::make_unique<logistics::device::ImageUploader>(mqtt_config.image_upload);
    }
    auto device_status = std::make_shared<logistics::device::DeviceStatus>(device_id);
    logistics::vision::VisionMqttWorkflow mqtt_workflow(device_id);
    logistics::device::MqttNodeClient mqtt_client(std::move(mqtt_config), "vision", device_status);
    mqtt_client.SetCommandHandler(
        [&mqtt_workflow, device_status](const logistics::contracts::mqtt::MqttMessage& message) {
            if (mqtt_workflow.AssignWork(message)) {
                const auto* work =
                    logistics::contracts::mqtt::GetPayload<logistics::contracts::mqtt::WorkCreatedPayload>(message);
                device_status->SetJobId(work->work_id);
                device_status->SetCurrentState("WORK_ASSIGNED");
            }
        });
    if (!mqtt_client.Start()) {
        return 1;
    }
    const std::string mqtt_session_id = logistics::device::GenerateMessageSessionId();
    std::uint64_t mqtt_sequence = 1;
    auto next_heartbeat = Clock::now();
    cv::Mat pending_capture;
    bool camera_error_reported = false;
#endif

    constexpr char kWindowName[] = "vision-node camera";
    cv::namedWindow(kWindowName, cv::WINDOW_AUTOSIZE);

    cv::VideoCapture camera;
    logistics::vision::DetectionModule detection_module;
    LatencyTracker latency_tracker;
    std::unordered_set<std::string> reported_barcodes;
    cv::Mat frame;
    int consecutive_frame_errors = 0;
    int exit_code = 0;
    auto last_latency_log = Clock::now();

    std::cout << "Camera settings: " << settings.width << 'x' << settings.height << " @ " << settings.fps << " FPS\n"
              << "Press q or Esc to exit.\n";

    bool should_exit = false;
    while (!should_exit) {
        if (!camera.isOpened()) {
            if (!OpenCamera(camera, settings)) {
                std::cerr << "Failed to open camera. Retrying in " << kReconnectIntervalMs << " ms.\n";
#ifdef LOGISTICS_VISION_MQTT_ENABLED
                if (!camera_error_reported && mqtt_client.IsConnected()) {
                    camera_error_reported = mqtt_client.PublishError(MakeVisionError(
                        device_id, logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++),
                        logistics::device::CurrentIso8601Timestamp(), "CAMERA_DISCONNECTED", "CAMERA_ERROR",
                        "failed to open camera"));
                    device_status->SetCurrentState("CAMERA_ERROR");
                    device_status->SetErrorCode("CAMERA_DISCONNECTED");
                }
#endif
                ShowCameraError(settings, "CAMERA: disconnected", kWindowName);
                should_exit = WaitForReconnectOrExit();
                continue;
            }

            consecutive_frame_errors = 0;
            std::cout << "Camera connected.\n";
#ifdef LOGISTICS_VISION_MQTT_ENABLED
            camera_error_reported = false;
            device_status->SetCurrentState("IDLE");
            device_status->SetErrorCode(std::nullopt);
#endif
        }

#ifdef LOGISTICS_VISION_MQTT_ENABLED
        const auto loop_now = Clock::now();
        if (mqtt_client.IsConnected() && loop_now >= next_heartbeat) {
            static_cast<void>(mqtt_client.PublishHeartbeat(
                logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++),
                logistics::device::CurrentIso8601Timestamp()));
            next_heartbeat = loop_now + logistics::contracts::mqtt::kHeartbeatInterval;
        }
#endif

        const auto capture_started = Clock::now();
        if (!camera.read(frame) || frame.empty()) {
            ++consecutive_frame_errors;
            std::cerr << "Camera frame error " << consecutive_frame_errors << '/' << kMaximumConsecutiveFrameErrors
                      << ".\n";

            if (consecutive_frame_errors >= kMaximumConsecutiveFrameErrors) {
                camera.release();
                ShowCameraError(settings, "CAMERA: frame stream lost", kWindowName);
                should_exit = WaitForReconnectOrExit();
            }
            continue;
        }
        const auto frame_received = Clock::now();
        consecutive_frame_errors = 0;

        const logistics::vision::DetectionResult detection_result = detection_module.Process(frame);
        const auto processing_finished = Clock::now();

        const LatencyMetrics current_latency{
            ToMilliseconds(frame_received - capture_started),
            ToMilliseconds(processing_finished - frame_received),
            ToMilliseconds(processing_finished - capture_started),
        };
        latency_tracker.Update(current_latency);

#ifdef LOGISTICS_VISION_MQTT_ENABLED
        std::optional<logistics::vision::VisionObservation> observation;
        if (detection_result.box.has_value()) {
            observation = MakeObservation(frame, detection_result,
                                          "capture-" + mqtt_session_id + '-' + std::to_string(mqtt_sequence) + ".jpg");
        }
        auto box_event = mqtt_workflow.Observe(
            std::move(observation), logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++),
            logistics::device::CurrentIso8601Timestamp());
        if (box_event.has_value()) {
            if (mqtt_client.PublishEvent(*box_event)) {
                pending_capture = frame.clone();
                device_status->SetCurrentState("AWAITING_WORK_ID");
            } else {
                mqtt_workflow.CancelPendingWork();
            }
        }

        if (auto work = mqtt_workflow.TakeAssignedWork(); work.has_value()) {
            const std::string timestamp = logistics::device::CurrentIso8601Timestamp();
            const auto position = logistics::vision::MakePositionDetectedMessage(
                device_id, *work, logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++),
                timestamp);
            const auto barcode = logistics::vision::MakeBarcodeDetectedMessage(
                device_id, *work, logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++),
                timestamp);
            const bool position_published = mqtt_client.PublishEvent(position);
            const bool barcode_published = mqtt_client.PublishEvent(barcode);
            bool image_published = image_uploader == nullptr;
            if (image_uploader != nullptr && !pending_capture.empty()) {
                std::vector<std::uint8_t> jpeg;
                if (cv::imencode(".jpg", pending_capture, jpeg, { cv::IMWRITE_JPEG_QUALITY, 90 })) {
                    const std::string upload_message_id =
                        logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++);
                    const std::string captured_at = logistics::device::CurrentIso8601Timestamp();
                    const auto uploaded =
                        image_uploader->Upload(device_id, work->work_id, upload_message_id, captured_at,
                                               work->observation.image_name, "image/jpeg", jpeg);
                    if (uploaded.IsConfirmed()) {
                        const auto image = logistics::vision::MakeProductImageMessage(
                            device_id, work->work_id, uploaded.upload_id, uploaded.path, uploaded.checksum,
                            logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++), captured_at);
                        image_published = mqtt_client.PublishEvent(image);
                    } else {
                        static_cast<void>(mqtt_client.PublishError(MakeVisionError(
                            device_id, logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++),
                            captured_at, "IMAGE_UPLOAD_FAILED", "UPLOAD_ERROR", uploaded.error, work->work_id)));
                    }
                } else {
                    static_cast<void>(mqtt_client.PublishError(MakeVisionError(
                        device_id, logistics::device::MakeMessageId(device_id, mqtt_session_id, mqtt_sequence++),
                        logistics::device::CurrentIso8601Timestamp(), "IMAGE_ENCODING_FAILED", "VISION_ERROR",
                        "failed to encode the captured frame as JPEG", work->work_id)));
                }
            }
            const bool all_published = position_published && barcode_published && image_published;
            device_status->SetCurrentState(all_published ? "VISION_REPORTED" : "VISION_ERROR");
            if (!all_published) {
                device_status->SetErrorCode("VISION_EVENT_PUBLISH_FAILED");
            } else {
                device_status->SetErrorCode(std::nullopt);
            }
            mqtt_workflow.CompleteWork();
            pending_capture.release();
        }
#endif

        DrawDetectionResult(frame, detection_result);
        DrawLatency(frame, latency_tracker.average());

        for (const logistics::vision::DetectedBarcode& barcode : detection_result.barcodes) {
            if (reported_barcodes.insert(barcode.value).second) {
                std::cout << "Barcode detected: " << barcode.type << ": " << barcode.value << '\n';
            }
        }

        if (processing_finished - last_latency_log >= std::chrono::seconds(1)) {
            const LatencyMetrics& average = latency_tracker.average();
            std::cout << std::fixed << std::setprecision(1) << "Latency: capture=" << average.capture_ms
                      << " ms, detect=" << average.processing_ms << " ms, total=" << average.total_ms << " ms\n";
            last_latency_log = processing_finished;
        }

        cv::imshow(kWindowName, frame);
        should_exit = IsExitKey(cv::waitKey(kWaitKeyDelayMs));
    }

    camera.release();
    cv::destroyAllWindows();
#ifdef LOGISTICS_VISION_MQTT_ENABLED
    mqtt_client.Stop();
#endif
    return exit_code;
}
