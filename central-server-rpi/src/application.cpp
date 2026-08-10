#include "logistics/central_server/application.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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
#include "logistics/central_server/process_state_store.hpp"
#include "logistics/central_server/server_config.hpp"
#include "logistics/central_server/work_invalidation.hpp"
#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {
namespace {

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
    try {
        server_config = LoadServerConfig(config_path);
    } catch (const ConfigError& error) {
        std::cerr << "[server][ERROR] server configuration failed: " << error.what() << '\n';
        return 2;
    }

    Database database;

    auto database_status = database.Open(server_config.database);
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

    RetentionService retention(database, server_config.storage, server_config.http.upload_root);
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
    MqttHandler mqtt_handler(*device_manager, {}, &persistence, server_config.process.default_destination);
    CommandManager command_manager;
    ProcessOrchestrator process_orchestrator(server_config.process);
    ProcessStateStore process_state_store(database);
    // ponytail: one process lock is enough at current throughput; split command/timeout execution only if measured.
    std::mutex process_mutex;

    std::optional<StoredProcessState> stored_process_state;
    std::vector<InvalidatedRestoredWork> invalidated_restored_works;
    database_status = process_state_store.Load(stored_process_state);
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] process state load failed: " << database_status.message << '\n';
        return 5;
    }
    if (stored_process_state.has_value()) {
        auto restore = process_orchestrator.RestoreAfterServerRestart(
            stored_process_state->system_state, std::move(stored_process_state->works),
            std::move(stored_process_state->gripper_targets), stored_process_state->message_sequence);
        if (!restore.restored) {
            std::cerr << "[server][ERROR] stored process state is invalid\n";
            return 5;
        }
        invalidated_restored_works = std::move(restore.invalidated_works);
    }
    std::vector<WorkInvalidation> work_invalidations;
    work_invalidations.reserve(invalidated_restored_works.size());
    for (const auto& restored : invalidated_restored_works) {
        work_invalidations.push_back({
            .work_id = restored.work_id,
            .message_id = "RECALIBRATION-" + restored.work_id,
            .error_code = "ERR-PROCESS-RECALIBRATION-REQUIRED",
            .reason = restored.reason,
            .cause = "CALIBRATION_CHANGED",
            .occurred_at_ms = CurrentUnixTimeMilliseconds(),
        });
    }
    for (const auto& invalidation : work_invalidations) {
        std::cerr << "[server][ERROR] restored work requires detection with the current calibration; work_id="
                  << invalidation.work_id << "; error=ERR-PROCESS-RECALIBRATION-REQUIRED\n";
        database_status = persistence.RecordWorkInvalidation(invalidation);
        if (!database_status.ok()) {
            std::cerr << "[server][ERROR] invalidated work persistence failed; work_id=" << invalidation.work_id
                      << "; message=" << database_status.message << '\n';
        }
    }

    bool process_state_persistence_healthy = true;
    const auto persist_process_state = [&process_orchestrator, &process_state_store,
                                        &process_state_persistence_healthy]() {
        DatabaseStatus status;
        for (int attempt = 0; attempt < 3; ++attempt) {
            status = process_state_store.Save(process_orchestrator.StateMachine().SystemState(),
                                              process_orchestrator.MessageSequence(),
                                              process_orchestrator.StateMachine().ActiveWorks(),
                                              process_orchestrator.GripperTargets(), CurrentUnixTimeMilliseconds());
            if (status.ok()) {
                return true;
            }
            if (!status.retryable()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
        }
        std::cerr << "[server][ERROR] process state persistence failed: " << status.message << '\n';
        process_state_persistence_healthy = false;
        stop_requested = 1;
        return false;
    };
    if (!persist_process_state()) {
        return 5;
    }

    MqttClient mqtt_client(std::move(mqtt_config), CreateMosquittoTransport());

    mqtt_handler.SetWorkCreatedHandler([&mqtt_client, &process_orchestrator, &persist_process_state,
                                        qt_client_id = server_config.qt_client_id](std::string_view device_id,
                                                                                   std::string_view work_id) {
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
        if (!persist_process_state()) {
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
            static_cast<void>(persist_process_state());
        } else {
            const auto assigned = process_orchestrator.ConfirmVisionAssignment(message.message_id, work_id);
            if (assigned.disposition == TransitionDisposition::kRejected) {
                std::cerr << "[server][ERROR] vision assignment transition rejected: " << assigned.reason << '\n';
                return false;
            }
            if (!persist_process_state()) {
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
    ProcessCommandTracker process_command_tracker;
    mqtt_handler.SetQtResponseHandler([&command_manager, &process_orchestrator, &active_system_recovery_request_id,
                                       &process_command_tracker,
                                       &publish_qt_response](const contracts::mqtt::MqttMessage& message) {
        const auto decision = command_manager.HandleResponse(message);
        switch (decision.disposition) {
            case CommandResponseDisposition::kForward: {
                if (!decision.message.has_value()) {
                    return false;
                }
                const auto* response =
                    contracts::mqtt::GetPayload<contracts::mqtt::CommandResponsePayload>(*decision.message);
                if (const auto failed_intent = process_command_tracker.HandleResponse(*decision.message)) {
                    const auto transition = process_orchestrator.FailDispatch(
                        *failed_intent, response == nullptr ? "process command failed" : response->message);
                    if (!transition.Applied()) {
                        std::cerr << "[server][ERROR] process command failure transition rejected: "
                                  << transition.reason << '\n';
                        return false;
                    }
                }
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

    const auto dispatch_command = [&mqtt_client, &mqtt_handler, &device_manager, &command_manager,
                                   &process_orchestrator, &server_config, &active_system_recovery_request_id,
                                   &process_state_persistence_healthy, &persist_process_state,
                                   &publish_qt_response](const contracts::mqtt::MqttMessage& message) {
        if (!process_state_persistence_healthy) {
            const auto rejected = command_manager.MakeImmediateResult(
                message, contracts::mqtt::CommandResult::kRejected, CurrentIso8601Timestamp(),
                std::string("ERR-PROCESS-STATE-PERSISTENCE"), "process state persistence is unavailable");
            return rejected.has_value() && publish_qt_response(*rejected);
        }
        if (const auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(message);
            command != nullptr && command->command == contracts::mqtt::ControlCommand::kStatusRequest &&
            command->component_id == contracts::mqtt::kCentralSnapshotComponentId) {
            const bool replayed =
                mqtt_handler.ReplayDeviceStatuses(command->target_device_id, CurrentIso8601Timestamp());
            const auto response = command_manager.MakeImmediateResult(
                message,
                replayed ? contracts::mqtt::CommandResult::kSuccess : contracts::mqtt::CommandResult::kRejected,
                CurrentIso8601Timestamp(),
                replayed ? std::nullopt : std::optional<std::string>{ "ERR-STATUS-SNAPSHOT-NOT-FOUND" },
                replayed ? "latest device status snapshot replayed" : "no retained device status snapshot found");
            return response.has_value() && publish_qt_response(*response);
        }

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
        const auto commit_system_command = [&process_orchestrator, &system_command, &persist_process_state]() {
            if (!system_command.has_value()) {
                return true;
            }
            const auto transition = process_orchestrator.ApplySystemCommand(*system_command);
            if (transition.disposition == TransitionDisposition::kRejected) {
                std::cerr << "[server][ERROR] system process command commit failed: " << transition.reason << '\n';
                return false;
            }
            return persist_process_state();
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

        const auto publish_to_device = [&mqtt_client, &message, &server_config](std::string_view device_id) {
            const auto forwarded =
                PrepareCommandForDevice(message, device_id, server_config.process.line_tracer_device_id,
                                        server_config.process.line_tracer_initial_position);
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
    mqtt_handler.SetProcessMessageHandler([&process_orchestrator, &device_manager, &dispatch_command,
                                           &process_command_tracker, &process_state_persistence_healthy,
                                           &persist_process_state](const contracts::mqtt::MqttMessage& message) {
        if (!process_state_persistence_healthy) {
            return false;
        }
        const auto result = process_orchestrator.Handle(message);
        if (!result.handled) {
            return true;
        }
        if (result.transition.disposition == TransitionDisposition::kRejected) {
            std::cerr << "[server][ERROR] process transition commit rejected: " << result.transition.reason << '\n';
            return false;
        }
        if (!persist_process_state()) {
            return false;
        }
        for (const auto& intent : result.commands) {
            if (!ResolveCommandTargets(intent.message, device_manager->RegisteredDevices()).IsValid()) {
                static_cast<void>(process_orchestrator.FailDispatch(intent, "target process node is unavailable"));
                static_cast<void>(persist_process_state());
                std::cerr << "[server][ERROR] process command target is unavailable: " << intent.work_id << '\n';
                return false;
            }
            if (!dispatch_command(intent.message)) {
                static_cast<void>(process_orchestrator.FailDispatch(intent, "process command publication failed"));
                static_cast<void>(persist_process_state());
                return false;
            }
            const auto confirmed = process_orchestrator.ConfirmDispatch(intent);
            if (!confirmed.Applied()) {
                std::cerr << "[server][ERROR] process dispatch transition rejected: " << confirmed.reason << '\n';
                return false;
            }
            if (!persist_process_state()) {
                static_cast<void>(process_orchestrator.FailDispatch(intent, "process state persistence failed"));
                static_cast<void>(persist_process_state());
                return false;
            }
            if (!process_command_tracker.Track(intent)) {
                static_cast<void>(process_orchestrator.FailDispatch(intent, "process command tracking failed"));
                static_cast<void>(persist_process_state());
                return false;
            }
        }
        return true;
    });

    mqtt_client.SetMessageHandler([&mqtt_handler, &process_orchestrator, &persist_process_state, &process_mutex](
                                      std::string_view topic, std::string_view payload) {
        const std::lock_guard process_lock(process_mutex);
        const auto previous_revision = process_orchestrator.Revision();
        if (!mqtt_handler.Handle(topic, payload)) {
            std::cerr << "[server][ERROR] MQTT message processing failed; topic=" << topic << '\n';
        }
        if (process_orchestrator.Revision() != previous_revision) {
            if (!persist_process_state()) {
                std::cerr << "[server][ERROR] automatic processing stopped because process state could not be saved\n";
            }
        }
    });

    stop_requested = 0;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (!mqtt_client.Start()) {
        std::cerr << "[server][ERROR] MQTT client startup failed\n";
        return 6;
    }
    const auto publish_recalibration_notifications = [&work_invalidations, &mqtt_client, &server_config]() {
        bool published = true;
        for (const auto& invalidation : work_invalidations) {
            const auto error = MakeWorkInvalidationError("central-server", invalidation, CurrentIso8601Timestamp());
            if (!mqtt_client.PublishMessage(contracts::mqtt::QtErrorTopic(server_config.qt_client_id), error,
                                            contracts::mqtt::Qos::kAtLeastOnce)) {
                std::cerr << "[server][ERROR] recalibration notification publish failed; work_id="
                          << invalidation.work_id << '\n';
                published = false;
            }
        }
        return published;
    };
    bool recalibration_notifications_pending = !work_invalidations.empty();

    Database upload_database;
    database_status = upload_database.Open(server_config.database);
    if (!database_status.ok()) {
        mqtt_client.Stop();
        std::cerr << "[server][ERROR] HTTP upload database open failed: " << database_status.message << '\n';
        return 7;
    }

    Database maintenance_database;
    database_status = maintenance_database.Open(server_config.database);
    if (!database_status.ok()) {
        mqtt_client.Stop();
        static_cast<void>(upload_database.Close());
        std::cerr << "[server][ERROR] maintenance database open failed: " << database_status.message << '\n';
        return 7;
    }
    RetentionService scheduled_retention(maintenance_database, server_config.storage, server_config.http.upload_root);
    auto next_retention_cleanup =
        std::chrono::steady_clock::now() + std::chrono::hours(server_config.storage.cleanup_interval_hours);

    HttpUploadServer upload_server(upload_database, server_config.http);
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
        const auto loop_now = std::chrono::steady_clock::now();
        if (loop_now >= next_retention_cleanup) {
            next_retention_cleanup = loop_now + std::chrono::hours(server_config.storage.cleanup_interval_hours);
            const auto retention_status = scheduled_retention.RunOnce(CurrentUnixTimeMilliseconds());
            if (!retention_status.ok()) {
                std::cerr << "[server][ERROR] scheduled retention cleanup failed: " << retention_status.message << '\n';
            }
        }
        if (recalibration_notifications_pending && mqtt_client.IsConnected()) {
            recalibration_notifications_pending = !publish_recalibration_notifications();
        }
        {
            const std::lock_guard process_lock(process_mutex);
            const auto previous_revision = process_orchestrator.Revision();
            static_cast<void>(mqtt_handler.CheckHeartbeatTimeouts());
            if (process_orchestrator.Revision() != previous_revision) {
                if (!persist_process_state()) {
                    std::cerr
                        << "[server][ERROR] automatic processing stopped because process state could not be saved\n";
                }
            }
        }
        for (const auto& timeout : command_manager.CheckTimeouts(CurrentIso8601Timestamp())) {
            {
                const std::lock_guard process_lock(process_mutex);
                const auto previous_revision = process_orchestrator.Revision();
                if (const auto failed_intent = process_command_tracker.HandleResponse(timeout)) {
                    const auto* response =
                        contracts::mqtt::GetPayload<contracts::mqtt::CommandResponsePayload>(timeout);
                    static_cast<void>(process_orchestrator.FailDispatch(
                        *failed_intent, response == nullptr ? "process command timed out" : response->message));
                }
                if (process_orchestrator.Revision() != previous_revision) {
                    static_cast<void>(persist_process_state());
                }
            }
            if (!publish_qt_response(timeout)) {
                std::cerr << "[server][ERROR] command timeout publish failed\n";
            }
        }
    }

    upload_server.Stop();
    mqtt_client.Stop();

    bool shutdown_ok = persist_process_state();
    database_status = maintenance_database.Close();
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] maintenance database close failed during shutdown: " << database_status.message
                  << '\n';
        shutdown_ok = false;
    }
    database_status = upload_database.Close();
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] HTTP upload database close failed during shutdown: " << database_status.message
                  << '\n';
        shutdown_ok = false;
    }
    database_status = database.Checkpoint();
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] database checkpoint failed during shutdown: " << database_status.message << '\n';
        shutdown_ok = false;
    }
    database_status = database.Close();
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] database close failed during shutdown: " << database_status.message << '\n';
        shutdown_ok = false;
    }

    std::clog << "[server][INFO] central server stopped" << (shutdown_ok ? " cleanly\n" : " with errors\n");

    return shutdown_ok ? 0 : 8;
}

}  // namespace logistics::central_server
