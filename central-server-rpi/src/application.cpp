#include "logistics/central_server/application.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "logistics/central_server/command_manager.hpp"
#include "logistics/central_server/database.hpp"
#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/http_upload_server.hpp"
#include "logistics/central_server/mqtt_client.hpp"
#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/central_server/mqtt_handler.hpp"
#include "logistics/central_server/mqtt_transport.hpp"
#include "logistics/central_server/persistence.hpp"
#include "logistics/central_server/process_orchestrator.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace logistics::central_server {
namespace {

struct ServerConfig {
    DatabaseConfig database;
    StorageConfig storage;
    HttpUploadServerConfig http;
    ProcessOrchestratorConfig process;
    std::string qt_client_id{ "control-center" };
};

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) {
    stop_requested = 1;
}

std::string CurrentIso8601Timestamp() {
    const std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc_time{};
    {
        static std::mutex time_mutex;
        std::lock_guard lock(time_mutex);
        const std::tm* converted = std::gmtime(&current_time);
        if (converted == nullptr) {
            return {};
        }
        utc_time = *converted;
    }
    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseInteger(std::string_view text, int& output) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, output);

    return result.ec == std::errc{} && result.ptr == end && output >= 0;
}

bool ParseBoolean(std::string_view text, bool& output) {
    if (text == "true" || text == "1") {
        output = true;
        return true;
    }
    if (text == "false" || text == "0") {
        output = false;
        return true;
    }
    return false;
}

bool ResolveConfigPath(int argc, char* argv[], std::filesystem::path& config_path) {
    if (argc == 1) {
        if (const char* environment_path = std::getenv("LOGISTICS_CENTRAL_SERVER_CONFIG");
            environment_path != nullptr && *environment_path != '\0') {
            config_path = environment_path;
        } else {
            config_path = std::filesystem::path("config") / "server.ini";
        }

        return true;
    }

    if (argc == 2 && argv[1] != nullptr) {
        const std::string_view argument{ argv[1] };

        if (argument.starts_with("--config=")) {
            const auto value = argument.substr(std::string_view("--config=").size());

            if (value.empty()) {
                return false;
            }

            config_path = std::string(value);
            return true;
        }

        if (!argument.empty() && !argument.starts_with("--")) {
            config_path = std::string(argument);
            return true;
        }

        return false;
    }

    if (argc == 3 && argv[1] != nullptr && argv[2] != nullptr && std::string_view(argv[1]) == "--config" &&
        std::string_view(argv[2]).size() != 0) {
        config_path = argv[2];
        return true;
    }

    return false;
}

DatabaseStatus LoadServerConfig(const std::filesystem::path& path, ServerConfig& config) {
    std::ifstream input(path);
    if (!input) {
        return {
            DatabaseStatusCode::kIoError,
            "cannot open config: " + path.string(),
        };
    }

    std::string section;
    std::string line;
    int line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        line = Trim(std::move(line));

        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }

        // MQTT와 device_registry 설정은 LoadMqttConfig가 처리합니다.
        if (section != "database" && section != "storage" && section != "http" && section != "routing" &&
            section != "process") {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            return {
                DatabaseStatusCode::kInvalidArgument,
                "invalid config line " + std::to_string(line_number),
            };
        }

        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));

        if (key.empty()) {
            return {
                DatabaseStatusCode::kInvalidArgument,
                "empty config key at line " + std::to_string(line_number),
            };
        }

        if (section == "database" && key == "path") {
            config.database.path = value;
            continue;
        }

        if (section == "database" && key == "migration_dir") {
            config.database.migration_dir = value;
            continue;
        }

        if (section == "storage" && key == "image_root") {
            config.storage.image_root = value;
            continue;
        }

        if (section == "http" && key == "tls_certificate") {
            config.http.tls_certificate = value;
            continue;
        }
        if (section == "http" && key == "tls_private_key") {
            config.http.tls_private_key = value;
            continue;
        }
        if (section == "http" && key == "bearer_token") {
            config.http.bearer_token = value;
            continue;
        }
        if (section == "http" && key == "upload_root") {
            config.http.upload_root = value;
            continue;
        }
        if (section == "routing" && key == "qt_client_id") {
            if (!contracts::mqtt::IsValidTopicLevel(value)) {
                return { DatabaseStatusCode::kInvalidArgument,
                         "invalid qt_client_id at config line " + std::to_string(line_number) };
            }
            config.qt_client_id = value;
            continue;
        }
        if (section == "process" &&
            (key == "server_id" || key == "input_device_id" || key == "vision_device_id" ||
             key == "gripper_device_id" || key == "sorting_device_id" || key == "line_tracer_device_id")) {
            if (!contracts::mqtt::IsValidTopicLevel(value)) {
                return { DatabaseStatusCode::kInvalidArgument,
                         "invalid process device ID at config line " + std::to_string(line_number) };
            }
            if (key == "server_id") {
                config.process.server_id = value;
            } else if (key == "input_device_id") {
                config.process.input_device_id = value;
            } else if (key == "vision_device_id") {
                config.process.vision_device_id = value;
            } else if (key == "gripper_device_id") {
                config.process.gripper_device_id = value;
            } else if (key == "sorting_device_id") {
                config.process.sorting_device_id = value;
            } else {
                config.process.line_tracer_device_id = value;
            }
            continue;
        }
        if ((section == "http" && (key == "enabled" || key == "tls_enabled")) ||
            (section == "process" && key == "enabled")) {
            bool parsed = false;
            if (!ParseBoolean(value, parsed)) {
                return { DatabaseStatusCode::kInvalidArgument,
                         "invalid boolean at config line " + std::to_string(line_number) };
            }
            if (section == "process") {
                config.process.enabled = parsed;
            } else if (key == "enabled") {
                config.http.enabled = parsed;
            } else {
                config.http.tls_enabled = parsed;
            }
            continue;
        }

        // log_root는 현재 StorageConfig에 대응 필드가 없으므로
        // 다른 로깅 모듈에서 처리하도록 무시합니다.
        if (section == "storage" && key == "log_root") {
            continue;
        }

        const bool recognized_integer = (section == "database" && key == "busy_timeout_ms") ||
                                        (section == "storage" && key == "cleanup_interval_hours") ||
                                        (section == "storage" && key == "mqtt_retention_days") ||
                                        (section == "storage" && key == "device_status_retention_days") ||
                                        (section == "storage" && key == "error_retention_days") ||
                                        (section == "storage" && key == "security_retention_days") ||
                                        (section == "storage" && key == "image_retention_days") ||
                                        (section == "http" && key == "port");

        if (!recognized_integer) {
            // 다른 기능이 추가한 설정과 병합 가능하도록
            // 알 수 없는 키는 여기에서 거부하지 않습니다.
            continue;
        }

        int parsed = 0;
        if (!ParseInteger(value, parsed)) {
            return {
                DatabaseStatusCode::kInvalidArgument,
                "invalid integer at config line " + std::to_string(line_number),
            };
        }

        if (section == "database") {
            config.database.busy_timeout_ms = parsed;
        } else if (section == "http") {
            config.http.port = parsed;
        } else if (key == "cleanup_interval_hours") {
            config.storage.cleanup_interval_hours = parsed;
        } else if (key == "mqtt_retention_days") {
            config.storage.mqtt_retention_days = parsed;
        } else if (key == "device_status_retention_days") {
            config.storage.device_status_retention_days = parsed;
        } else if (key == "error_retention_days") {
            config.storage.error_retention_days = parsed;
        } else if (key == "security_retention_days") {
            config.storage.security_retention_days = parsed;
        } else if (key == "image_retention_days") {
            config.storage.image_retention_days = parsed;
        }
    }

    return DatabaseStatus::Ok();
}

}  // namespace

int Application::Run(int argc, char* argv[]) {
    std::filesystem::path config_path;

    if (!ResolveConfigPath(argc, argv, config_path)) {
        std::cerr << "usage: logistics_central_server [server.ini]\n"
                  << "   or: logistics_central_server --config <server.ini>\n"
                  << "   or: logistics_central_server --config=<server.ini>\n";
        return 2;
    }

    MqttConfig mqtt_config;

    try {
        mqtt_config = LoadMqttConfig(config_path);
    } catch (const ConfigError& error) {
        std::cerr << "[server][ERROR] MQTT configuration failed: " << error.what() << '\n';
        return 2;
    }

    ServerConfig server_config;
    auto database_status = LoadServerConfig(config_path, server_config);

    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] server configuration failed: " << database_status.message << '\n';
        return 2;
    }

    Database database;

    database_status = database.Open(server_config.database);
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] database open failed: " << database_status.message << '\n';
        return 3;
    }

    database_status = MigrationRunner::Apply(database, server_config.database.migration_dir);
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] database migration failed: " << database_status.message << '\n';
        return 3;
    }

    database_status = database.IntegrityCheck();
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] database integrity check failed: " << database_status.message << '\n';
        return 3;
    }

    RetentionService retention(database, server_config.storage);
    database_status = retention.RunOnce(CurrentUnixTimeMilliseconds());

    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] retention cleanup failed: " << database_status.message << '\n';
        return 4;
    }

    std::unique_ptr<DeviceManager> device_manager;

    try {
        device_manager = std::make_unique<DeviceManager>(mqtt_config.device_registry_path);
    } catch (const DeviceRegistryError& error) {
        std::cerr << "[server][ERROR] device registry failed: " << error.what() << '\n';
        return 5;
    }

    PersistenceService persistence(database, server_config.storage);
    MqttHandler mqtt_handler(*device_manager, {}, &persistence);
    CommandManager command_manager;
    ProcessOrchestrator process_orchestrator(server_config.process);

    MqttClient mqtt_client(std::move(mqtt_config), CreateMosquittoTransport());

    mqtt_handler.SetWorkCreatedHandler([&mqtt_client, &process_orchestrator, qt_client_id = server_config.qt_client_id](
                                           std::string_view device_id, std::string_view work_id) {
        const contracts::mqtt::MqttMessage message{
            .protocol_version = std::string(contracts::mqtt::kCurrentProtocolVersion),
            .message_id = "WORK-" + std::string(work_id),
            .message_type = contracts::mqtt::MessageType::kWorkCreated,
            .source_id = "central-server",
            .timestamp = CurrentIso8601Timestamp(),
            .data = contracts::mqtt::WorkCreatedPayload{ .work_id = std::string(work_id) },
        };
        const auto transition = process_orchestrator.BeginWork(message.message_id, work_id, device_id);
        if (transition.disposition == TransitionDisposition::kRejected) {
            std::cerr << "[server][ERROR] WORK_CREATED process transition rejected: " << transition.reason << '\n';
            return false;
        }
        const std::string_view vision_device_id =
            process_orchestrator.Enabled() ? process_orchestrator.VisionDeviceId() : device_id;
        const bool sent_to_device = mqtt_client.PublishMessage(contracts::mqtt::DeviceCommandTopic(vision_device_id),
                                                               message, contracts::mqtt::Qos::kAtLeastOnce);
        const bool sent_to_qt = mqtt_client.PublishMessage(contracts::mqtt::QtEventTopic(qt_client_id), message,
                                                           contracts::mqtt::Qos::kAtLeastOnce);
        if (!sent_to_device || !sent_to_qt) {
            const ProcessCommandIntent failed_intent{
                .message = message,
                .dispatched_event = ProcessEventType::kWorkCreated,
                .work_id = std::string(work_id),
            };
            static_cast<void>(process_orchestrator.FailDispatch(failed_intent, "WORK_CREATED MQTT publication failed"));
        } else {
            const auto assigned = process_orchestrator.ConfirmVisionAssignment(message.message_id, work_id);
            if (assigned.disposition == TransitionDisposition::kRejected) {
                std::cerr << "[server][ERROR] vision assignment transition rejected: " << assigned.reason << '\n';
                return false;
            }
        }
        return sent_to_device && sent_to_qt;
    });
    mqtt_handler.SetQtEventHandler(
        [&mqtt_client, qt_client_id = server_config.qt_client_id](const contracts::mqtt::MqttMessage& message) {
            return mqtt_client.PublishMessage(contracts::mqtt::QtEventTopic(qt_client_id), message,
                                              contracts::mqtt::Qos::kAtLeastOnce);
        });
    const auto publish_qt_response =
        [&mqtt_client, qt_client_id = server_config.qt_client_id](const contracts::mqtt::MqttMessage& message) {
            return mqtt_client.PublishMessage(contracts::mqtt::QtResponseTopic(qt_client_id), message,
                                              contracts::mqtt::Qos::kAtLeastOnce);
        };
    std::optional<std::string> active_system_recovery_request_id;
    mqtt_handler.SetQtResponseHandler([&command_manager, &process_orchestrator, &active_system_recovery_request_id,
                                       &publish_qt_response](const contracts::mqtt::MqttMessage& message) {
        const auto decision = command_manager.HandleResponse(message);
        switch (decision.disposition) {
            case CommandResponseDisposition::kForward: {
                if (!decision.message.has_value()) {
                    return false;
                }
                const auto* response =
                    contracts::mqtt::GetPayload<contracts::mqtt::CommandResponsePayload>(*decision.message);
                if (response != nullptr && response->command == contracts::mqtt::ControlCommand::kRecovery &&
                    response->result == contracts::mqtt::CommandResult::kSuccess &&
                    active_system_recovery_request_id == response->request_id) {
                    const auto transition = process_orchestrator.CompleteSystemRecovery();
                    if (transition.disposition == TransitionDisposition::kRejected) {
                        std::cerr << "[server][ERROR] system recovery completion failed: " << transition.reason << '\n';
                        return false;
                    }
                    active_system_recovery_request_id.reset();
                } else if (response != nullptr && response->command == contracts::mqtt::ControlCommand::kRecovery &&
                           contracts::mqtt::IsTerminal(response->result) &&
                           active_system_recovery_request_id == response->request_id) {
                    active_system_recovery_request_id.reset();
                }
                return publish_qt_response(*decision.message);
            }
            case CommandResponseDisposition::kDuplicate:
                std::clog << "[server][INFO] duplicate command response ignored: " << decision.reason << '\n';
                return true;
            case CommandResponseDisposition::kUnknownRequest:
                std::clog << "[server][INFO] late or unknown command response ignored: " << decision.reason << '\n';
                return true;
            case CommandResponseDisposition::kRejected:
                std::cerr << "[server][ERROR] command response rejected: " << decision.reason << '\n';
                return false;
        }
        return false;
    });
    mqtt_handler.SetQtStatusHandler(
        [&mqtt_client, qt_client_id = server_config.qt_client_id](const contracts::mqtt::MqttMessage& message) {
            return mqtt_client.PublishMessage(contracts::mqtt::QtStatusTopic(qt_client_id), message,
                                              contracts::mqtt::Qos::kAtLeastOnce);
        });
    mqtt_handler.SetQtErrorHandler(
        [&mqtt_client, qt_client_id = server_config.qt_client_id](const contracts::mqtt::MqttMessage& message) {
            return mqtt_client.PublishMessage(contracts::mqtt::QtErrorTopic(qt_client_id), message,
                                              contracts::mqtt::Qos::kAtLeastOnce);
        });
    mqtt_handler.SetProcessMessageGuard([&process_orchestrator](const contracts::mqtt::MqttMessage& message) {
        const auto preview = process_orchestrator.Preview(message);
        if (!preview.handled || preview.transition.disposition != TransitionDisposition::kRejected) {
            return true;
        }
        std::cerr << "[server][ERROR] invalid process transition: " << preview.transition.reason << '\n';
        return false;
    });

    const auto dispatch_command = [&mqtt_client, &device_manager, &command_manager, &process_orchestrator,
                                   &active_system_recovery_request_id,
                                   &publish_qt_response](const contracts::mqtt::MqttMessage& message) {
        std::optional<contracts::mqtt::ControlCommand> system_command;
        if (const auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(message);
            command != nullptr && (command->target_device_id == "SYSTEM" || command->target_device_id == "ALL")) {
            system_command = command->command;
        } else if (const auto* emergency = contracts::mqtt::GetPayload<contracts::mqtt::EmergencyStopPayload>(message);
                   emergency != nullptr) {
            system_command = emergency->command;
        }
        if (system_command.has_value()) {
            const auto preview = process_orchestrator.PreviewSystemCommand(*system_command);
            if (preview.disposition == TransitionDisposition::kRejected) {
                const auto rejected = command_manager.MakeImmediateResult(
                    message, contracts::mqtt::CommandResult::kRejected, CurrentIso8601Timestamp(),
                    std::string("ERR-PROCESS-STATE"), preview.reason);
                return rejected.has_value() && publish_qt_response(*rejected);
            }
        }
        const auto commit_system_command = [&process_orchestrator, &system_command]() {
            if (!system_command.has_value()) {
                return true;
            }
            const auto transition = process_orchestrator.ApplySystemCommand(*system_command);
            if (transition.disposition == TransitionDisposition::kRejected) {
                std::cerr << "[server][ERROR] system process command commit failed: " << transition.reason << '\n';
                return false;
            }
            return true;
        };

        const auto route = ResolveCommandTargets(message, device_manager->RegisteredDevices());
        if (!route.IsValid()) {
            std::cerr << "[server][ERROR] command has no reachable target devices\n";
            const auto rejected = command_manager.MakeImmediateResult(
                message, contracts::mqtt::CommandResult::kRejected, CurrentIso8601Timestamp(),
                std::string("ERR-COMMAND-NO-TARGET"), "command has no reachable target devices");
            return rejected.has_value() && publish_qt_response(*rejected);
        }

        std::string request_id;
        if (const auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(message)) {
            request_id = command->request_id;
        } else if (const auto* emergency_stop =
                       contracts::mqtt::GetPayload<contracts::mqtt::EmergencyStopPayload>(message)) {
            request_id = emergency_stop->request_id;
        } else if (const auto* destination =
                       contracts::mqtt::GetPayload<contracts::mqtt::DestinationSetPayload>(message)) {
            request_id = destination->request_id;
        }
        if (!command_manager.TrackCommand(message, route.target_device_ids)) {
            std::clog << "[server][INFO] duplicate command request ignored: " << command_manager.LastError() << '\n';
            return true;
        }

        if (route.broadcast) {
            auto forwarded = message;
            auto* payload = contracts::mqtt::GetPayload<contracts::mqtt::EmergencyStopPayload>(forwarded);
            if (payload == nullptr) {
                return false;
            }
            payload->target_device_id = "ALL";
            if (mqtt_client.PublishMessage(contracts::mqtt::kSystemBroadcastCommandTopic, forwarded,
                                           contracts::mqtt::Qos::kAtLeastOnce)) {
                return commit_system_command();
            }
            const auto failed =
                command_manager.HandleDispatchFailures(request_id, route.target_device_ids, CurrentIso8601Timestamp());
            return failed.has_value() && publish_qt_response(*failed);
        }

        const auto publish_to_device = [&mqtt_client, &message](std::string_view device_id) {
            auto forwarded = message;
            forwarded.message_id += "-" + std::string(device_id);
            if (auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(forwarded)) {
                command->target_device_id = device_id;
            } else if (auto* destination =
                           contracts::mqtt::GetPayload<contracts::mqtt::DestinationSetPayload>(forwarded)) {
                destination->target_device_id = device_id;
            }
            return mqtt_client.PublishMessage(contracts::mqtt::DeviceCommandTopic(device_id), forwarded,
                                              contracts::mqtt::Qos::kAtLeastOnce);
        };

        std::vector<std::string> failed_devices;
        for (const auto& device_id : route.target_device_ids) {
            if (!publish_to_device(device_id)) {
                failed_devices.push_back(device_id);
            }
        }
        if (failed_devices.empty()) {
            const bool committed = commit_system_command();
            if (committed && system_command == contracts::mqtt::ControlCommand::kRecovery) {
                active_system_recovery_request_id = request_id;
            }
            return committed;
        }
        const auto failure =
            command_manager.HandleDispatchFailures(request_id, failed_devices, CurrentIso8601Timestamp());
        return failure.has_value() && publish_qt_response(*failure);
    };
    mqtt_handler.SetCommandRouteHandler(dispatch_command);
    mqtt_handler.SetProcessMessageHandler(
        [&process_orchestrator, &device_manager, &dispatch_command](const contracts::mqtt::MqttMessage& message) {
            const auto result = process_orchestrator.Handle(message);
            if (!result.handled) {
                return true;
            }
            if (result.transition.disposition == TransitionDisposition::kRejected) {
                std::cerr << "[server][ERROR] process transition commit rejected: " << result.transition.reason << '\n';
                return false;
            }
            for (const auto& intent : result.commands) {
                if (!ResolveCommandTargets(intent.message, device_manager->RegisteredDevices()).IsValid()) {
                    static_cast<void>(process_orchestrator.FailDispatch(intent, "target process node is unavailable"));
                    std::cerr << "[server][ERROR] process command target is unavailable: " << intent.work_id << '\n';
                    return false;
                }
                if (!dispatch_command(intent.message)) {
                    static_cast<void>(process_orchestrator.FailDispatch(intent, "process command publication failed"));
                    return false;
                }
                const auto confirmed = process_orchestrator.ConfirmDispatch(intent);
                if (!confirmed.Applied()) {
                    std::cerr << "[server][ERROR] process dispatch transition rejected: " << confirmed.reason << '\n';
                    return false;
                }
            }
            return true;
        });

    mqtt_client.SetMessageHandler([&mqtt_handler](std::string_view topic, std::string_view payload) {
        static_cast<void>(mqtt_handler.Handle(topic, payload));
    });

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (!mqtt_client.Start()) {
        std::cerr << "[server][ERROR] MQTT client startup failed\n";
        return 6;
    }

    HttpUploadServer upload_server(database, server_config.http);
    database_status = upload_server.Start();
    if (!database_status.ok()) {
        mqtt_client.Stop();
        std::cerr << "[server][ERROR] HTTP upload server startup failed: " << database_status.message << '\n';
        return 7;
    }

    std::clog << "[server][INFO] central server started; "
              << "registered devices=" << device_manager->RegisteredDeviceCount()
              << "; database=" << server_config.database.path.string() << "; http_upload="
              << (server_config.http.enabled ? std::to_string(server_config.http.port) : std::string("disabled"))
              << "; process_orchestrator=" << (process_orchestrator.Enabled() ? "enabled" : "disabled") << '\n';

    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        static_cast<void>(mqtt_handler.CheckHeartbeatTimeouts());
        for (const auto& timeout : command_manager.CheckTimeouts(CurrentIso8601Timestamp())) {
            if (!publish_qt_response(timeout)) {
                std::cerr << "[server][ERROR] command timeout publish failed\n";
            }
        }
    }

    upload_server.Stop();
    mqtt_client.Stop();

    std::clog << "[server][INFO] central server stopped\n";

    return 0;
}

}  // namespace logistics::central_server
