#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
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
#include <thread>
#include <unordered_set>
#include <vector>

#include "detection.hpp"
#include "failure_frame_store.hpp"
#include "image_upload_task.hpp"
#include "vision_mqtt_workflow.hpp"
#include "vision_processing_config.hpp"
#include "vision_state_transaction.hpp"

#ifdef LOGISTICS_VISION_MQTT_ENABLED
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/device/device_control_state.hpp"
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
#ifdef LOGISTICS_VISION_MQTT_ENABLED
constexpr std::string_view kWaitingForProductState = "WAITING_FOR_PRODUCT";
#endif
const cv::Scalar kBoxOutlineColor{ 255, 128, 0 };
const cv::Scalar kBarcodeBoxColor{ 0, 255, 0 };
const cv::Scalar kErrorColor{ 0, 0, 255 };
const cv::Scalar kTextColor{ 255, 255, 255 };

struct CameraSettings {
    int width = 1280;
    int height = 720;
    int fps = 30;
    bool headless = false;
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
    std::cout << "Usage: " << executable << " [--width N] [--height N] [--fps N] [--headless] [--config node.ini]\n"
              << "Defaults: --width 1280 --height 720 --fps 30 --config device-rpi/config/node.ini\n";
}

bool HasGraphicalDisplay() {
    const char* display = std::getenv("DISPLAY");
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && *display != '\0') || (wayland_display != nullptr && *wayland_display != '\0');
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

        if (argument == "--headless") {
            settings.headless = true;
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

bool WaitForReconnectOrExit(const bool headless) {
    if (headless) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectIntervalMs));
        return false;
    }
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

void DrawOperatingState(cv::Mat& frame, const std::string_view state) {
    cv::putText(frame, "VISION: " + std::string(state), cv::Point(20, 35), cv::FONT_HERSHEY_SIMPLEX, 0.8, kTextColor,
                2);
}

logistics::vision::VisionObservation MakeObservation(const cv::Mat& frame,
                                                     const logistics::vision::DetectionResult& result,
                                                     std::string image_name) {
    cv::Rect box;
    std::array<logistics::contracts::mqtt::PixelPoint, kBarcodeCornerCount> box_corners{};
    if (result.box.has_value()) {
        box = result.box->roi;
        cv::Point2f detected_corners[kBarcodeCornerCount];
        result.box->outline.points(detected_corners);
        for (std::size_t index = 0; index < box_corners.size(); ++index) {
            box_corners[index] = {
                .x = static_cast<double>(detected_corners[index].x),
                .y = static_cast<double>(detected_corners[index].y),
            };
        }
    }
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
        .box_corners = box_corners,
        .barcode = std::move(barcode),
        .barcode_region_detected = result.diagnostics.barcode_region_detected,
        .box_detected = result.box.has_value(),
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

struct ImageUploadCompletion final {
    logistics::vision::AssignedVisionWork work;
    std::vector<logistics::vision::VisionPublication> publications;
    std::string captured_at;
    logistics::device::ImageUploadResult result;
    std::uint64_t generation{};
    bool encoded{};
};

struct ImageUploadIntent final {
    logistics::vision::AssignedVisionWork work;
    std::string upload_message_id;
    std::string captured_at;
    cv::Mat frame;
    std::uint64_t generation{};
};
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
    if (!settings.headless && !HasGraphicalDisplay()) {
        settings.headless = true;
        std::clog << "No graphical display detected; running in headless mode.\n";
    }
    logistics::vision::VisionProcessingConfig vision_processing_config;
    try {
        vision_processing_config = logistics::vision::LoadVisionProcessingConfig(settings.config_path);
    } catch (const logistics::vision::VisionProcessingConfigError& error) {
        std::cerr << "[vision][ERROR] " << error.what() << '\n';
        return 2;
    }
    logistics::vision::FailureFrameStore failure_frame_store(vision_processing_config.failure_frame_capture);

#ifdef LOGISTICS_VISION_MQTT_ENABLED
    logistics::device::MqttNodeConfig mqtt_config;
    try {
        mqtt_config = logistics::device::LoadMqttNodeConfig(settings.config_path);
    } catch (const logistics::device::NodeConfigError& error) {
        std::cerr << "[vision][ERROR] " << error.what() << '\n';
        return 2;
    }
    const std::string device_id = mqtt_config.device_id;
    std::shared_ptr<logistics::device::ImageUploader> image_uploader;
    if (mqtt_config.image_upload_enabled) {
        image_uploader = std::make_shared<logistics::device::ImageUploader>(mqtt_config.image_upload);
    }
    logistics::vision::ImageUploadTask<ImageUploadCompletion> pending_image_upload;
    logistics::vision::ImageUploadRetryState<ImageUploadIntent> image_upload_retry;
    std::optional<std::string> pending_image_upload_work_id;
    logistics::vision::VisionStateTransaction vision_state;
    auto device_status = std::make_shared<logistics::device::DeviceStatus>(device_id);
    logistics::vision::VisionMqttWorkflow mqtt_workflow(
        device_id, 3, 5, std::chrono::milliseconds(vision_processing_config.preassignment_timeout_ms),
        std::chrono::milliseconds(vision_processing_config.barcode_timeout_ms));
    logistics::vision::VisionResultOutbox result_outbox;
    logistics::vision::PendingWorkFrame pending_capture;
    logistics::device::DeviceControlState control_state({
        .device_id = device_id,
        .component_name = "vision",
        .not_ready_error_code = "ERR-CAMERA-UNAVAILABLE",
    });
    logistics::device::MqttNodeClient mqtt_client(std::move(mqtt_config), "vision", device_status);
    mqtt_client.SetWorkCreatedEpochReassignmentGuard([&mqtt_workflow] { return mqtt_workflow.CanAcceptWork(); });
    const std::string mqtt_session_id = logistics::device::GenerateMessageSessionId();
    std::atomic_uint64_t mqtt_sequence{ 1 };
    const auto flush_result_outbox = [&mqtt_client, &result_outbox]() {
        return result_outbox.Flush(
            [&mqtt_client](const logistics::contracts::mqtt::MqttMessage& message) {
                return mqtt_client.PublishEvent(message);
            },
            [&mqtt_client](const logistics::contracts::mqtt::MqttMessage& message) {
                return mqtt_client.PublishError(message);
            });
    };
    const auto complete_work = [&mqtt_workflow, &pending_capture, &device_status, &control_state]() {
        mqtt_workflow.CompleteWork();
        pending_capture.Reset();
        device_status->SetJobId(std::nullopt);
        if (control_state.IsOperational()) {
            device_status->SetCurrentState(std::string(kWaitingForProductState));
            device_status->SetErrorCode(std::nullopt);
        } else {
            device_status->SetCurrentState(control_state.CurrentState());
        }
    };
    const auto start_pending_image_upload = [&]() {
        if (!control_state.IsOperational() || image_uploader == nullptr ||
            !image_upload_retry.ReadyToSchedule(result_outbox.PendingWorkId().has_value(),
                                                pending_image_upload.Active())) {
            return false;
        }
        const ImageUploadIntent* pending_intent = image_upload_retry.PendingIntent();
        if (pending_intent == nullptr) {
            return false;
        }
        ImageUploadIntent intent = *pending_intent;
        const std::string work_id = intent.work.work_id;
        const bool started = pending_image_upload.Start(
            [uploader = image_uploader, device_id, intent = std::move(intent)](std::stop_token stop_token) mutable {
                ImageUploadCompletion completion{
                    .work = std::move(intent.work),
                    .publications = {},
                    .captured_at = intent.captured_at,
                    .result = {},
                    .generation = intent.generation,
                };
                try {
                    std::vector<std::uint8_t> jpeg;
                    completion.encoded = cv::imencode(".jpg", intent.frame, jpeg, { cv::IMWRITE_JPEG_QUALITY, 90 });
                    if (completion.encoded) {
                        completion.result = uploader->Upload(
                            device_id, completion.work.work_id, intent.upload_message_id, intent.captured_at,
                            completion.work.observation->image_name, "image/jpeg", jpeg, stop_token);
                    }
                } catch (const std::exception& error) {
                    completion.result.error = error.what();
                }
                return completion;
            });
        if (!started) {
            return false;
        }
        static_cast<void>(image_upload_retry.MarkScheduled(work_id));
        pending_image_upload_work_id = work_id;
        pending_capture.Reset();
        device_status->SetCurrentState("UPLOAD_PENDING");
        return true;
    };
    device_status->SetCurrentState(control_state.CurrentState());
    mqtt_client.SetCommandHandler([&mqtt_workflow, &result_outbox, &image_upload_retry, &control_state, &mqtt_client,
                                   &mqtt_sequence, &mqtt_session_id, &device_id, &vision_state,
                                   device_status](const logistics::contracts::mqtt::MqttMessage& message) {
        const std::string response_message_id = logistics::device::MakeMessageId(
            device_id, mqtt_session_id, mqtt_sequence.fetch_add(1, std::memory_order_relaxed));
        std::optional<logistics::device::DeviceControlDecision> decision;
        std::optional<bool> ignored_message_result;
        vision_state.Synchronize([&](auto& state) {
            decision =
                control_state.HandleCommand(message, response_message_id, logistics::device::CurrentIso8601Timestamp());
            if (!decision.has_value()) {
                if (logistics::contracts::mqtt::GetPayload<logistics::contracts::mqtt::WorkCreatedPayload>(message) !=
                    nullptr) {
                    const bool assigned = mqtt_workflow.AssignWork(message);
                    if (assigned) {
                        const auto* work =
                            logistics::contracts::mqtt::GetPayload<logistics::contracts::mqtt::WorkCreatedPayload>(
                                message);
                        device_status->SetJobId(work->work_id);
                        device_status->SetCurrentState("WORK_ASSIGNED");
                        device_status->SetErrorCode(std::nullopt);
                        std::clog << "[vision][control][INFO] WORK_CREATED assigned for image capture; work_id="
                                  << work->work_id << '\n';
                    } else {
                        std::clog << "[vision][control][INFO] WORK_CREATED ignored; another image work is active\n";
                    }
                    // WORK_CREATED is an idempotent assignment, not a control
                    // command requiring a response. Consume it even when a
                    // newer work is already active so central replay cannot
                    // poison the node's inbound command journal.
                    ignored_message_result = true;
                } else {
                    ignored_message_result = false;
                }
                return;
            }
            if (decision->clear_work) {
                state.ClearWork();
                mqtt_workflow.Reset();
                result_outbox.Reset();
                image_upload_retry.Reset();
                device_status->SetJobId(std::nullopt);
            }
            if (decision->state_changed) {
                device_status->SetCurrentState(control_state.IsOperational() ? std::string(kWaitingForProductState)
                                                                             : control_state.CurrentState());
                device_status->SetErrorCode(std::nullopt);
            }
        });
        if (decision.has_value()) {
            if (const auto* response =
                    logistics::contracts::mqtt::GetPayload<logistics::contracts::mqtt::CommandResponsePayload>(
                        decision->response);
                response != nullptr) {
                std::clog << "[vision][control][INFO] command="
                          << logistics::contracts::mqtt::ToString(response->command)
                          << "; result=" << logistics::contracts::mqtt::ToString(response->result)
                          << "; state=" << control_state.CurrentState();
                if (response->error_code.has_value()) {
                    std::clog << "; error=" << *response->error_code;
                }
                std::clog << "; message=" << response->message << '\n';
            }
            if (!mqtt_client.PublishResponse(decision->response)) {
                std::cerr << "[vision][mqtt][ERROR] failed to publish command response\n";
                return false;
            }
            return true;
        }
        return ignored_message_result.value_or(false);
    });
    if (!mqtt_client.Start()) {
        return 1;
    }
    auto next_heartbeat = Clock::now();
    bool camera_error_reported = false;
#endif

    constexpr char kWindowName[] = "vision-node camera";
    if (!settings.headless) {
        cv::namedWindow(kWindowName, cv::WINDOW_AUTOSIZE);
    }

    cv::VideoCapture camera;
    std::unique_ptr<logistics::vision::DetectionModule> detection_module;
    try {
        detection_module = std::make_unique<logistics::vision::DetectionModule>(std::move(vision_processing_config));
    } catch (const std::exception& error) {
        std::cerr << "[vision][ERROR] failed to initialize vision processing: " << error.what() << '\n';
#ifdef LOGISTICS_VISION_MQTT_ENABLED
        mqtt_client.Stop();
#endif
        return 2;
    }
    LatencyTracker latency_tracker;
    std::unordered_set<std::string> reported_barcodes;
    cv::Mat frame;
    int consecutive_frame_errors = 0;
    int exit_code = 0;
    bool super_resolution_error_reported = false;
    auto last_latency_log = Clock::now();

    std::cout << "Camera settings: " << settings.width << 'x' << settings.height << " @ " << settings.fps << " FPS\n";
    if (settings.headless) {
        std::cout << "Headless mode enabled; stop with Ctrl+C or SIGTERM.\n";
    } else {
        std::cout << "Press q or Esc to exit.\n";
    }

    bool should_exit = false;
    while (!should_exit) {
#ifdef LOGISTICS_VISION_MQTT_ENABLED
        const bool reset_requested = vision_state.Synchronize([&](auto&) {
            if (!control_state.ConsumeResetRequest()) {
                return false;
            }
            control_state.SetReady(false);
            mqtt_workflow.Reset();
            result_outbox.Reset();
            image_upload_retry.Reset();
            return true;
        });
        if (reset_requested) {
            camera.release();
            pending_capture.Reset();
            pending_image_upload.Cancel();
            pending_image_upload_work_id.reset();
            device_status->SetJobId(std::nullopt);
            device_status->SetCurrentState(control_state.CurrentState());
        }
#endif
        if (!camera.isOpened()) {
            if (!OpenCamera(camera, settings)) {
                std::cerr << "Failed to open camera. Retrying in " << kReconnectIntervalMs << " ms.\n";
#ifdef LOGISTICS_VISION_MQTT_ENABLED
                vision_state.Synchronize([&](auto&) { control_state.SetReady(false); });
                if (!camera_error_reported) {
                    camera_error_reported = mqtt_client.PublishError(MakeVisionError(
                        device_id,
                        logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                         mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                        logistics::device::CurrentIso8601Timestamp(), "ERR-VISION-CAMERA-OPEN-FAILED", "CAMERA_ERROR",
                        "failed to open camera"));
                    const std::string control_status = control_state.CurrentState();
                    device_status->SetCurrentState(control_status == "STOPPED" ? "CAMERA_ERROR" : control_status);
                    device_status->SetErrorCode("ERR-VISION-CAMERA-OPEN-FAILED");
                }
#endif
                if (!settings.headless) {
                    ShowCameraError(settings, "CAMERA: disconnected", kWindowName);
                }
                should_exit = WaitForReconnectOrExit(settings.headless);
                continue;
            }

            consecutive_frame_errors = 0;
            std::cout << "Camera connected.\n";
#ifdef LOGISTICS_VISION_MQTT_ENABLED
            camera_error_reported = false;
            device_status->SetErrorCode(std::nullopt);
            auto recovery = vision_state.Synchronize([&](auto&) {
                control_state.SetReady(true);
                return control_state.CompleteRecovery(
                    logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                     mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                    logistics::device::CurrentIso8601Timestamp());
            });
            device_status->SetCurrentState(control_state.CurrentState());
            if (recovery.has_value()) {
                if (!mqtt_client.PublishResponse(*recovery)) {
                    std::cerr << "[vision][mqtt][ERROR] failed to publish recovery completion\n";
                } else {
                    std::clog << "[vision][control][INFO] recovery completed; state=" << control_state.CurrentState()
                              << '\n';
                }
            }
#endif
        }

#ifdef LOGISTICS_VISION_MQTT_ENABLED
        const auto loop_now = Clock::now();
        const bool can_complete_upload =
            pending_image_upload.Active() && pending_image_upload.Ready() && vision_state.Synchronize([&](auto&) {
                return control_state.IsOperational() && !result_outbox.PendingWorkId().has_value();
            });
        if (can_complete_upload) {
            ImageUploadCompletion completion = pending_image_upload.Take();
            pending_image_upload_work_id.reset();
            const bool published = vision_state.PublishIfCurrent(
                completion.generation, [&control_state] { return control_state.IsOperational(); },
                [&] {
                    if (!completion.encoded) {
                        completion.publications.push_back({
                            logistics::vision::VisionPublicationChannel::kError,
                            MakeVisionError(
                                device_id,
                                logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                                 mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                                completion.captured_at, "ERR-VISION-IMAGE-ENCODING-FAILED", "VISION_ERROR",
                                completion.result.error.empty() ? "failed to encode the captured frame as JPEG"
                                                                : completion.result.error,
                                completion.work.work_id),
                        });
                    } else if (completion.result.IsConfirmed()) {
                        std::clog << "[vision][transport][INFO] HTTP image upload confirmed; work_id="
                                  << completion.work.work_id << '\n'
                                  << std::flush;
                        completion.publications.push_back({
                            logistics::vision::VisionPublicationChannel::kEvent,
                            logistics::vision::MakeProductImageMessage(
                                device_id, completion.work.work_id, completion.result.upload_id, completion.result.path,
                                completion.result.checksum,
                                logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                                 mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                                completion.captured_at),
                        });
                    } else {
                        std::cerr << "[vision][ERROR] image upload failed: " << completion.result.error << '\n';
                        completion.publications.push_back({
                            logistics::vision::VisionPublicationChannel::kError,
                            MakeVisionError(
                                device_id,
                                logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                                 mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                                completion.captured_at, "ERR-VISION-IMAGE-UPLOAD-FAILED", "UPLOAD_ERROR",
                                completion.result.error, completion.work.work_id),
                        });
                    }

                    const bool queued =
                        result_outbox.Enqueue(completion.work.work_id, std::move(completion.publications));
                    const bool durable = queued && flush_result_outbox();
                    if (durable) {
                        complete_work();
                    } else {
                        device_status->SetCurrentState("RESULT_PENDING");
                        device_status->SetErrorCode(
                            queued ? std::nullopt : std::optional<std::string>{ "ERR-VISION-RESULT-QUEUE-FAILED" });
                    }
                });
            if (!published) {
                std::clog << "[vision][transport][INFO] discarded upload result for cleared work; work_id="
                          << completion.work.work_id << '\n';
            }
        }
        if (mqtt_client.IsConnected() && loop_now >= next_heartbeat) {
            static_cast<void>(mqtt_client.PublishHeartbeat(
                logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                 mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                logistics::device::CurrentIso8601Timestamp()));
            next_heartbeat = loop_now + logistics::contracts::mqtt::kHeartbeatInterval;
        }
        vision_state.Synchronize([&](auto&) {
            if (const auto pending_work_id = result_outbox.PendingWorkId();
                control_state.IsOperational() && pending_work_id.has_value()) {
                const bool image_upload_pending =
                    (pending_image_upload_work_id.has_value() && *pending_image_upload_work_id == *pending_work_id) ||
                    image_upload_retry.PendingWorkId() == pending_work_id;
                if (result_outbox.Flush(
                        [&mqtt_client](const logistics::contracts::mqtt::MqttMessage& message) {
                            return mqtt_client.PublishEvent(message);
                        },
                        [&mqtt_client](const logistics::contracts::mqtt::MqttMessage& message) {
                            return mqtt_client.PublishError(message);
                        })) {
                    std::clog << "[vision][transport][INFO] MQTT result publication completed; work_id="
                              << *pending_work_id << '\n'
                              << std::flush;
                    if (image_upload_pending) {
                        device_status->SetCurrentState("UPLOAD_PENDING");
                    } else {
                        mqtt_workflow.CompleteWork();
                        pending_capture.Reset();
                        device_status->SetJobId(std::nullopt);
                        if (control_state.IsOperational()) {
                            device_status->SetCurrentState(std::string(kWaitingForProductState));
                            device_status->SetErrorCode(std::nullopt);
                        } else {
                            device_status->SetCurrentState(control_state.CurrentState());
                        }
                    }
                }
            }
            static_cast<void>(start_pending_image_upload());
        });
#endif

        const auto capture_started = Clock::now();
        if (!camera.read(frame) || frame.empty()) {
            ++consecutive_frame_errors;
            std::cerr << "Camera frame error " << consecutive_frame_errors << '/' << kMaximumConsecutiveFrameErrors
                      << ".\n";

            if (consecutive_frame_errors >= kMaximumConsecutiveFrameErrors) {
                camera.release();
#ifdef LOGISTICS_VISION_MQTT_ENABLED
                vision_state.Synchronize([&](auto&) { control_state.SetReady(false); });
                device_status->SetCurrentState(control_state.CurrentState());
                device_status->SetErrorCode("ERR-VISION-CAMERA-FRAME-UNAVAILABLE");
                camera_error_reported = mqtt_client.PublishError(MakeVisionError(
                    device_id,
                    logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                     mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                    logistics::device::CurrentIso8601Timestamp(), "ERR-VISION-CAMERA-FRAME-UNAVAILABLE", "CAMERA_ERROR",
                    "camera frame stream is unavailable"));
#endif
                if (!settings.headless) {
                    ShowCameraError(settings, "CAMERA: frame stream lost", kWindowName);
                }
                should_exit = WaitForReconnectOrExit(settings.headless);
            }
            continue;
        }
        const auto frame_received = Clock::now();
        consecutive_frame_errors = 0;

#ifdef LOGISTICS_VISION_MQTT_ENABLED
        if (!vision_state.Synchronize([&](auto&) { return control_state.IsOperational(); })) {
            if (!settings.headless) {
                DrawOperatingState(frame, control_state.CurrentState());
                cv::imshow(kWindowName, frame);
                should_exit = IsExitKey(cv::waitKey(kWaitKeyDelayMs));
            }
            continue;
        }
#endif

#ifdef LOGISTICS_VISION_MQTT_ENABLED
        const bool allow_expensive_fallback =
            vision_state.Synchronize([&](auto&) { return mqtt_workflow.NeedsBarcodeFallback(); });
#else
        constexpr bool allow_expensive_fallback = true;
#endif
        const logistics::vision::DetectionResult detection_result =
            detection_module->Process(frame, allow_expensive_fallback);
        if (detection_result.diagnostics.super_resolution_failed && !super_resolution_error_reported) {
            std::cerr << "[vision][WARN] super-resolution fallback failed; original-frame processing remains active\n";
            super_resolution_error_reported = true;
        } else if (!detection_result.diagnostics.super_resolution_failed) {
            super_resolution_error_reported = false;
        }
        const auto processing_finished = Clock::now();

        const LatencyMetrics current_latency{
            ToMilliseconds(frame_received - capture_started),
            ToMilliseconds(processing_finished - frame_received),
            ToMilliseconds(processing_finished - capture_started),
        };
        latency_tracker.Update(current_latency);

#ifdef LOGISTICS_VISION_MQTT_ENABLED
        std::optional<logistics::vision::VisionObservation> observation;
        if (detection_result.box.has_value() || detection_result.diagnostics.barcode_region_detected ||
            !detection_result.barcodes.empty()) {
            observation = MakeObservation(frame, detection_result,
                                          "capture-" + mqtt_session_id + '-' +
                                              std::to_string(mqtt_sequence.load(std::memory_order_relaxed)) + ".jpg");
        }
        vision_state.Synchronize([&](auto&) {
            if (!control_state.IsOperational()) {
                return;
            }
            mqtt_workflow.Observe(std::move(observation));
            pending_capture.Observe(frame, detection_result.box.has_value(), !detection_result.barcodes.empty(),
                                    mqtt_workflow.HasPendingBarcode() || mqtt_workflow.NeedsBarcodeFallback());
        });

        std::optional<logistics::vision::AssignedVisionWork> work;
        std::uint64_t generation{};
        vision_state.Synchronize([&](auto& state) {
            if (control_state.IsOperational()) {
                work = mqtt_workflow.TakeAssignedWork();
                generation = state.Generation();
            }
        });
        if (work.has_value()) {
            const std::string timestamp = logistics::device::CurrentIso8601Timestamp();
            std::vector<logistics::vision::VisionPublication> publications;
            if (work->observation.has_value()) {
                publications.push_back({
                    logistics::vision::VisionPublicationChannel::kEvent,
                    logistics::vision::MakePositionDetectedMessage(
                        device_id, *work,
                        logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                         mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                        timestamp),
                });
            }
            bool result_deferred = false;
            const bool barcode_detected = work->observation.has_value() && work->observation->barcode.has_value();
            if (barcode_detected) {
                publications.push_back({
                    logistics::vision::VisionPublicationChannel::kEvent,
                    logistics::vision::MakeBarcodeDetectedMessage(
                        device_id, *work,
                        logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                         mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                        timestamp),
                });
            } else {
                if (pending_capture.Empty()) {
                    std::cerr << "[vision][WARN] barcode recognition failed without a retained box frame; work_id="
                              << work->work_id << '\n';
                } else if (!failure_frame_store.Store(pending_capture.Frame(), work->work_id)) {
                    std::cerr << "[vision][WARN] failed to archive barcode recognition failure frame; work_id="
                              << work->work_id << '\n';
                }
                publications.push_back({
                    logistics::vision::VisionPublicationChannel::kEvent,
                    logistics::vision::MakeBarcodeDetectedMessage(
                        device_id, *work,
                        logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                         mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                        timestamp),
                });
            }
            if (barcode_detected && image_uploader != nullptr && !pending_capture.Empty()) {
                if (pending_image_upload.Active()) {
                    publications.push_back({
                        logistics::vision::VisionPublicationChannel::kError,
                        MakeVisionError(
                            device_id,
                            logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                             mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                            logistics::device::CurrentIso8601Timestamp(), "ERR-VISION-IMAGE-UPLOAD-BUSY",
                            "UPLOAD_ERROR", "the previous image upload is still running", work->work_id),
                    });
                } else {
                    const std::string upload_message_id = logistics::device::MakeMessageId(
                        device_id, mqtt_session_id, mqtt_sequence.fetch_add(1, std::memory_order_relaxed));
                    const std::string captured_at = logistics::device::CurrentIso8601Timestamp();
                    ImageUploadIntent upload_intent{
                        .work = *work,
                        .upload_message_id = upload_message_id,
                        .captured_at = captured_at,
                        .frame = pending_capture.Frame().clone(),
                        .generation = generation,
                    };
                    const bool current = vision_state.PublishIfCurrent(
                        generation, [&control_state] { return control_state.IsOperational(); },
                        [&] {
                            const bool retained = image_upload_retry.Retain(work->work_id, std::move(upload_intent));
                            if (retained) {
                                device_status->SetCurrentState("RESULT_PENDING");
                                device_status->SetErrorCode(std::nullopt);
                                result_deferred = true;
                            } else {
                                device_status->SetCurrentState("RESULT_PENDING");
                                device_status->SetErrorCode("ERR-VISION-IMAGE-UPLOAD-BUSY");
                            }
                        });
                    if (!current) {
                        continue;
                    }
                }
            } else if (barcode_detected && image_uploader != nullptr) {
                std::cerr << "[vision][ERROR] barcode was detected without a captured frame\n";
                publications.push_back({
                    logistics::vision::VisionPublicationChannel::kError,
                    MakeVisionError(
                        device_id,
                        logistics::device::MakeMessageId(device_id, mqtt_session_id,
                                                         mqtt_sequence.fetch_add(1, std::memory_order_relaxed)),
                        logistics::device::CurrentIso8601Timestamp(), "ERR-VISION-IMAGE-CAPTURE-MISSING",
                        "VISION_ERROR", "barcode was detected without a captured frame", work->work_id),
                });
            }
            if (result_deferred) {
                continue;
            }
            const bool current = vision_state.PublishIfCurrent(
                generation, [&control_state] { return control_state.IsOperational(); },
                [&] {
                    const bool queued = result_outbox.Enqueue(work->work_id, std::move(publications));
                    const bool durable = queued && flush_result_outbox();
                    if (durable) {
                        pending_capture.Reset();
                        complete_work();
                    } else {
                        device_status->SetCurrentState("RESULT_PENDING");
                        device_status->SetErrorCode(
                            queued ? std::nullopt : std::optional<std::string>{ "ERR-VISION-RESULT-QUEUE-FAILED" });
                    }
                });
            if (!current) {
                continue;
            }
        }
#endif

        if (!settings.headless) {
            DrawDetectionResult(frame, detection_result);
            DrawLatency(frame, latency_tracker.average());
        }

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

        if (!settings.headless) {
            cv::imshow(kWindowName, frame);
            should_exit = IsExitKey(cv::waitKey(kWaitKeyDelayMs));
        }
    }

    camera.release();
    if (!settings.headless) {
        cv::destroyAllWindows();
    }
#ifdef LOGISTICS_VISION_MQTT_ENABLED
    pending_image_upload.Cancel();
    mqtt_client.Stop();
#endif
    return exit_code;
}
