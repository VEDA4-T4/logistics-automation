#include "logistics/central_server/application.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
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
#include <unordered_set>
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
#include "logistics/central_server/vision_measurement_buffer.hpp"
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

std::optional<std::string> ProcessCommandRequestId(const contracts::mqtt::MqttMessage& message) {
    if (const auto* command = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(message)) {
        return command->request_id;
    }
    if (const auto* destination = contracts::mqtt::GetPayload<contracts::mqtt::DestinationSetPayload>(message)) {
        return destination->request_id;
    }
    return std::nullopt;
}

bool InputStationOccupied(const ProcessStateMachine& machine) {
    return std::ranges::any_of(machine.ActiveWorks(), [](const WorkProcessSnapshot& work) {
        const auto stage = work.suspended_stage.value_or(work.stage);
        return stage == WorkStage::kInputDetected || stage == WorkStage::kVisionAssigned ||
               stage == WorkStage::kVisionProcessing || stage == WorkStage::kBarcodeRecognized ||
               stage == WorkStage::kProductIdentified || stage == WorkStage::kGripperRequested ||
               stage == WorkStage::kGripperTransferring;
    });
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

    database_status = PrepareDatabaseForStartup(database, server_config.database);
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] database startup preparation failed: " << database_status.message << '\n';
        return 3;
    }

    if (server_config.database.startup_mode == StartupMode::kFresh) {
        std::cout << "[server][INFO] database reset; product catalog preserved\n";
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
    MqttHandler mqtt_handler(*device_manager, {}, &persistence, server_config.process.default_destination,
                             server_config.sensor_detection);
    CommandManager command_manager;
    std::unordered_map<std::string, contracts::mqtt::ControlCommand> pending_system_commands;
    ProcessOrchestrator process_orchestrator(server_config.process);
    ProcessCommandTracker process_command_tracker;
    std::unordered_set<std::string> restored_pending_process_commands;
    bool safe_restored_replay_pending = false;
    InputDetectionGate input_detection_gate(server_config.process.input_device_id);
    LineTracerLoadGate line_tracer_load_gate(server_config.process.line_tracer_device_id);
    ProcessStateStore process_state_store(database);
    // ponytail: one process lock is enough at current throughput; split command/timeout execution only if measured.
    std::mutex process_mutex;
    VisionMeasurementBuffer pending_vision_measurement;

    std::optional<StoredProcessState> stored_process_state;
    std::vector<PendingMqttDelivery> restored_mqtt_deliveries;
    std::vector<InvalidatedRestoredWork> invalidated_restored_works;
    database_status = process_state_store.Load(stored_process_state);
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] process state load failed: " << database_status.message << '\n';
        return 5;
    }
    const std::string process_epoch = stored_process_state.has_value() && !stored_process_state->process_epoch.empty()
                                          ? stored_process_state->process_epoch
                                          : GenerateProcessEpoch();
    process_orchestrator.SetProcessEpoch(process_epoch);
    mqtt_handler.SetProcessEpoch(process_epoch, server_config.database.startup_mode == StartupMode::kFresh);
    if (stored_process_state.has_value()) {
        auto pending_commands = std::move(stored_process_state->pending_commands);
        for (auto& intent : pending_commands) {
            auto stamped = StampProcessEpoch(std::move(intent.message), process_epoch);
            if (!stamped.has_value()) {
                std::cerr << "[server][ERROR] stored process command belongs to a stale process epoch\n";
                return 5;
            }
            intent.message = std::move(*stamped);
        }
        auto restore = process_orchestrator.RestoreAfterServerRestart(
            stored_process_state->system_state, std::move(stored_process_state->works),
            std::move(stored_process_state->gripper_targets), stored_process_state->message_sequence,
            std::move(stored_process_state->processed_message_ids));
        if (!restore.restored) {
            std::cerr << "[server][ERROR] stored process state is invalid\n";
            return 5;
        }
        invalidated_restored_works = std::move(restore.invalidated_works);
        std::erase_if(pending_commands, [&process_orchestrator](const ProcessCommandIntent& intent) {
            return !process_orchestrator.StateMachine().FindWork(intent.work_id).has_value();
        });
        if (!process_command_tracker.Restore(std::move(pending_commands))) {
            std::cerr << "[server][ERROR] stored process command outbox is invalid\n";
            return 5;
        }
        if (!command_manager.Restore(std::move(stored_process_state->command_manager))) {
            std::cerr << "[server][ERROR] stored command manager state is invalid\n";
            return 5;
        }
        pending_system_commands = std::move(stored_process_state->pending_system_commands);
        for (const auto& intent : process_command_tracker.PendingCommands()) {
            if (const auto request_id = ProcessCommandRequestId(intent.message)) {
                restored_pending_process_commands.insert(*request_id);
            }
            if (const auto* command =
                    contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(intent.message);
                command != nullptr && command->command == contracts::mqtt::ControlCommand::kStop) {
                safe_restored_replay_pending = true;
            }
        }
    }
    database_status = process_state_store.LoadPendingMqttDeliveries(restored_mqtt_deliveries);
    if (!database_status.ok()) {
        std::cerr << "[server][ERROR] stored MQTT delivery outbox is invalid: " << database_status.message << '\n';
        return 5;
    }
    std::unordered_set<std::string> restored_input_detected_work_ids;
    std::unordered_set<std::string> restored_vision_assigned_work_ids;
    if (process_orchestrator.StateMachine().SystemState() == ProcessSystemState::kStopped) {
        for (const auto& work : process_orchestrator.StateMachine().ActiveWorks()) {
            if (work.stage != WorkStage::kStopped || !work.suspended_stage.has_value()) {
                continue;
            }
            if (work.suspended_stage == WorkStage::kInputDetected) {
                restored_input_detected_work_ids.insert(work.work_id);
            } else if (work.suspended_stage == WorkStage::kVisionAssigned) {
                restored_vision_assigned_work_ids.insert(work.work_id);
            }
        }
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

    std::atomic_bool process_state_persistence_healthy{ true };
    std::mutex runtime_store_mutex;
    const auto run_runtime_store = [&process_state_persistence_healthy, &runtime_store_mutex](
                                       auto&& operation, std::string_view context) {
        if (!process_state_persistence_healthy.load()) {
            return false;
        }
        DatabaseStatus status;
        for (int attempt = 0; attempt < 3; ++attempt) {
            {
                const std::lock_guard store_lock(runtime_store_mutex);
                status = operation();
            }
            if (status.ok()) {
                return true;
            }
            if (!status.retryable()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
        }
        std::cerr << "[server][ERROR] " << context << " failed: " << status.message << '\n';
        process_state_persistence_healthy = false;
        stop_requested = 1;
        return false;
    };
    const auto persist_process_state = [&process_orchestrator, &process_command_tracker, &command_manager,
                                        &pending_system_commands, &process_state_store, &run_runtime_store,
                                        &process_epoch]() {
        const auto active_works = process_orchestrator.StateMachine().ActiveWorks();
        for (const auto& intent : process_command_tracker.PendingCommands()) {
            if (std::ranges::find(active_works, intent.work_id, &WorkProcessSnapshot::work_id) == active_works.end()) {
                const auto request_id = ProcessCommandRequestId(intent.message);
                if (request_id.has_value()) {
                    static_cast<void>(process_command_tracker.Remove(*request_id));
                }
            }
        }
        return run_runtime_store(
            [&] {
                return process_state_store.Save(
                    process_orchestrator.StateMachine().SystemState(), process_orchestrator.MessageSequence(),
                    active_works, process_orchestrator.GripperTargets(), process_command_tracker.PendingCommands(),
                    CurrentUnixTimeMilliseconds(), {}, process_orchestrator.StateMachine().ProcessedMessageIds(),
                    command_manager.Snapshot(), pending_system_commands, process_epoch);
            },
            "process state persistence");
    };
    if (!persist_process_state()) {
        return 5;
    }

    MqttClient mqtt_client(std::move(mqtt_config), CreateMosquittoTransport());
    std::mutex mqtt_delivery_mutex;
    std::unordered_set<std::string> mqtt_deliveries_in_flight;
    std::unordered_map<std::string, PendingMqttDelivery> mqtt_deliveries_acknowledged;
    mqtt_client.SetDisconnectedHandler([&mqtt_delivery_mutex, &mqtt_deliveries_in_flight] {
        const std::lock_guard delivery_lock(mqtt_delivery_mutex);
        mqtt_deliveries_in_flight.clear();
    });
    const auto delivery_key = [](std::string_view topic, std::string_view message_id) {
        return std::string(topic) + "\n" + std::string(message_id);
    };
    const auto publish_enqueued = [&mqtt_client, &mqtt_delivery_mutex, &mqtt_deliveries_in_flight,
                                   &mqtt_deliveries_acknowledged, &delivery_key](const PendingMqttDelivery& delivery) {
        if (!mqtt_client.IsReady()) {
            std::clog << "[server][MQTT][INFO] publish deferred; topic=" << delivery.topic
                      << "; messageId=" << delivery.message.message_id
                      << "; messageType=" << contracts::mqtt::ToString(delivery.message.message_type)
                      << "; reason=client-not-ready\n";
            return true;
        }
        const auto key = delivery_key(delivery.topic, delivery.message.message_id);
        {
            const std::lock_guard delivery_lock(mqtt_delivery_mutex);
            if (!mqtt_deliveries_in_flight.insert(key).second) {
                std::clog << "[server][MQTT][INFO] publish already in flight; topic=" << delivery.topic
                          << "; messageId=" << delivery.message.message_id << '\n';
                return true;
            }
        }
        const bool accepted = mqtt_client.PublishMessageAcknowledged(
            delivery.topic, delivery.message, contracts::mqtt::Qos::kAtLeastOnce,
            [&mqtt_delivery_mutex, &mqtt_deliveries_in_flight, &mqtt_deliveries_acknowledged, key, delivery](int) {
                const std::lock_guard delivery_lock(mqtt_delivery_mutex);
                mqtt_deliveries_in_flight.erase(key);
                mqtt_deliveries_acknowledged.insert_or_assign(key, delivery);
            });
        if (!accepted) {
            const std::lock_guard delivery_lock(mqtt_delivery_mutex);
            mqtt_deliveries_in_flight.erase(key);
        }
        return true;
    };
    const auto publish_durable = [&publish_enqueued, &process_state_store, &run_runtime_store, &process_epoch](
                                     std::string_view topic, const contracts::mqtt::MqttMessage& message) {
        const auto stamped = StampProcessEpoch(message, process_epoch);
        if (!stamped.has_value()) {
            std::cerr << "[server][ERROR] refusing MQTT delivery from a stale process epoch\n";
            return false;
        }
        if (!run_runtime_store(
                [&] { return process_state_store.EnqueueMqttDelivery(topic, *stamped, CurrentUnixTimeMilliseconds()); },
                "MQTT delivery outbox enqueue")) {
            return false;
        }
        return publish_enqueued(PendingMqttDelivery{ .topic = std::string(topic), .message = *stamped });
    };
    const auto pump_durable = [&mqtt_client, &publish_enqueued, &process_state_store, &run_runtime_store,
                               &mqtt_delivery_mutex, &mqtt_deliveries_acknowledged, &restored_input_detected_work_ids,
                               &restored_vision_assigned_work_ids, &delivery_key, &process_epoch,
                               vision_device_id = server_config.process.vision_device_id]() {
        std::vector<PendingMqttDelivery> acknowledged;
        {
            const std::lock_guard delivery_lock(mqtt_delivery_mutex);
            for (const auto& [_, delivery] : mqtt_deliveries_acknowledged) {
                acknowledged.push_back(delivery);
            }
        }
        for (const auto& delivery : acknowledged) {
            if (!run_runtime_store(
                    [&] { return process_state_store.RemoveMqttDelivery(delivery.topic, delivery.message.message_id); },
                    "MQTT delivery outbox acknowledgement")) {
                return false;
            }
            const std::lock_guard delivery_lock(mqtt_delivery_mutex);
            mqtt_deliveries_acknowledged.erase(delivery_key(delivery.topic, delivery.message.message_id));
        }
        std::vector<PendingMqttDelivery> pending;
        if (!run_runtime_store([&] { return process_state_store.LoadPendingMqttDeliveries(pending); },
                               "MQTT delivery outbox load")) {
            return false;
        }
        if (!mqtt_client.IsReady()) {
            return true;
        }
        for (auto delivery : pending) {
            const auto epoch_result = PreparePendingMqttDeliveryEpoch(
                delivery, process_epoch, [&](std::string_view topic, std::string_view message_id) {
                    return run_runtime_store([&] { return process_state_store.RemoveMqttDelivery(topic, message_id); },
                                             "stale MQTT delivery outbox removal");
                });
            if (epoch_result == PendingDeliveryEpochResult::kError) {
                return false;
            }
            if (epoch_result == PendingDeliveryEpochResult::kDropped) {
                std::cerr << "[server][ERROR] dropped MQTT delivery from a stale process epoch\n";
                continue;
            }
            static_cast<void>(publish_enqueued(delivery));
        }
        return true;
    };
    std::function<bool(const std::vector<ProcessCommandIntent>&)> dispatch_process_commands;
    std::function<bool(bool)> replay_restored_process_commands;

    mqtt_handler.SetWorkCreationSourceGuard([&process_orchestrator](std::string_view device_id) {
        return process_orchestrator.IsWorkCreationSource(device_id);
    });
    mqtt_handler.SetWorkCreatedHandler([&publish_enqueued, &process_orchestrator, &process_command_tracker,
                                        &persist_process_state, &process_state_store, &run_runtime_store,
                                        &dispatch_process_commands, &command_manager, &pending_system_commands,
                                        &process_epoch, qt_client_id = server_config.qt_client_id](
                                           std::string_view device_id, std::string_view work_id) {
        if (!process_orchestrator.IsWorkCreationSource(device_id) ||
            !process_orchestrator.StateMachine().AcceptsNewWork()) {
            return WorkCreationDisposition::kDiscarded;
        }
        const contracts::mqtt::MqttMessage message{
            .protocol_version = std::string(contracts::mqtt::kCurrentProtocolVersion),
            .message_id = "WORK-" + std::string(work_id),
            .message_type = contracts::mqtt::MessageType::kWorkCreated,
            .source_id = "central-server",
            .timestamp = CurrentIso8601Timestamp(),
            .process_epoch = process_epoch,
            .data = contracts::mqtt::WorkCreatedPayload{ .work_id = std::string(work_id) },
        };
        const auto begin = process_orchestrator.BeginWork(message.message_id, work_id, device_id, message.timestamp);
        const auto existing = process_orchestrator.StateMachine().FindWork(work_id);
        if (begin.transition.disposition == TransitionDisposition::kDuplicate) {
            // State and MQTT outbox are committed atomically.  A replay with no
            // active work was already completed/removed; a later stage already
            // has its WORK_CREATED delivery queued or acknowledged.
            if (!existing.has_value() || existing->stage != WorkStage::kInputDetected) {
                return WorkCreationDisposition::kCreated;
            }
        }
        if (begin.transition.disposition == TransitionDisposition::kRejected) {
            // The BOX event is durable and can be replayed after a dispatch-failure path
            // has already terminally reported this work.  Do not leave that inbox row
            // RECEIVED forever merely because BeginWork is no longer applicable.
            if (existing.has_value() &&
                (existing->stage == WorkStage::kFailed || existing->stage == WorkStage::kStopped ||
                 existing->stage == WorkStage::kEmergencyStopped)) {
                return WorkCreationDisposition::kDiscarded;
            }
            std::cerr << "[server][ERROR] WORK_CREATED process transition rejected: " << begin.transition.reason
                      << '\n';
            return WorkCreationDisposition::kFailed;
        }
        if (!dispatch_process_commands || !dispatch_process_commands(begin.commands)) {
            std::cerr << "[server][ERROR] input conveyor could not be stopped for workId=" << work_id << '\n';
            return WorkCreationDisposition::kFailed;
        }
        const auto qt_topic = contracts::mqtt::QtEventTopic(qt_client_id);
        const auto vision_topic = contracts::mqtt::DeviceCommandTopic(process_orchestrator.VisionDeviceId());
        const std::vector deliveries{
            PendingMqttDelivery{ .topic = vision_topic, .message = message },
            PendingMqttDelivery{ .topic = qt_topic, .message = message },
        };
        const auto assigned = process_orchestrator.ConfirmVisionAssignment(message.message_id, work_id);
        if (assigned.disposition == TransitionDisposition::kRejected) {
            return WorkCreationDisposition::kFailed;
        }
        if (!run_runtime_store(
                [&] {
                    return process_state_store.Save(
                        process_orchestrator.StateMachine().SystemState(), process_orchestrator.MessageSequence(),
                        process_orchestrator.StateMachine().ActiveWorks(), process_orchestrator.GripperTargets(),
                        process_command_tracker.PendingCommands(), CurrentUnixTimeMilliseconds(), deliveries,
                        process_orchestrator.StateMachine().ProcessedMessageIds(), command_manager.Snapshot(),
                        pending_system_commands, process_epoch);
                },
                "WORK_CREATED state/outbox persistence")) {
            return WorkCreationDisposition::kFailed;
        }
        bool published = true;
        for (const auto& delivery : deliveries) {
            published = publish_enqueued(delivery) && published;
        }
        return published ? WorkCreationDisposition::kCreated : WorkCreationDisposition::kFailed;
    });
    mqtt_handler.SetQtEventHandler([&mqtt_client, &publish_durable, qt_client_id = server_config.qt_client_id](
                                       const contracts::mqtt::MqttMessage& message) {
        if (contracts::mqtt::IsTransientTelemetry(message.message_type)) {
            static_cast<void>(
                mqtt_client.PublishTransientMessage(contracts::mqtt::QtEventTopic(qt_client_id), message));
            // A newer sample supersedes this one; a disconnected UI must not create a replay backlog.
            return true;
        }
        return publish_durable(contracts::mqtt::QtEventTopic(qt_client_id), message);
    });
    const auto publish_qt_response =
        [&publish_durable, qt_client_id = server_config.qt_client_id](const contracts::mqtt::MqttMessage& message) {
            return publish_durable(contracts::mqtt::QtResponseTopic(qt_client_id), message);
        };
    const auto publish_failed_work =
        [&publish_durable, qt_client_id = server_config.qt_client_id, source_id = server_config.process.server_id](
            std::string_view message_id, std::string_view work_id, std::string_view reason) {
            const auto completed =
                MakeWorkFailureCompletion(source_id, message_id, work_id, reason, CurrentIso8601Timestamp());
            return publish_durable(contracts::mqtt::QtEventTopic(qt_client_id), completed);
        };
    const auto commit_recovery_response =
        [&pending_system_commands, &process_orchestrator, &process_command_tracker, &command_manager,
         &process_state_store, &publish_enqueued, &run_runtime_store, &restored_input_detected_work_ids,
         &restored_vision_assigned_work_ids, &restored_pending_process_commands, &safe_restored_replay_pending,
         &process_epoch, qt_client_id = server_config.qt_client_id,
         source_id = server_config.process.server_id](const contracts::mqtt::MqttMessage& device_response) {
            if (!Application::CommitRecoveryResponse(
                    process_orchestrator, process_command_tracker, command_manager, pending_system_commands,
                    device_response, qt_client_id, source_id, CurrentIso8601Timestamp(), process_epoch,
                    [&](std::uint64_t process_sequence, std::uint64_t command_sequence,
                        const std::vector<PendingMqttDelivery>& completions) {
                        return run_runtime_store(
                            [&] {
                                return process_state_store.CommitRecovery(process_sequence, command_sequence,
                                                                          CurrentUnixTimeMilliseconds(), completions,
                                                                          process_epoch);
                            },
                            "recovery state/outbox persistence");
                    },
                    [&publish_enqueued](const PendingMqttDelivery& completion) {
                        static_cast<void>(publish_enqueued(completion));
                    })) {
                return false;
            }
            restored_pending_process_commands.clear();
            safe_restored_replay_pending = false;
            restored_input_detected_work_ids.clear();
            restored_vision_assigned_work_ids.clear();
            return true;
        };
    const auto finish_system_command = [&pending_system_commands, &process_orchestrator, &process_command_tracker,
                                        &command_manager, &process_state_store, &publish_enqueued,
                                        &replay_restored_process_commands, &persist_process_state, &run_runtime_store,
                                        &restored_input_detected_work_ids, &restored_vision_assigned_work_ids,
                                        &process_epoch, qt_client_id = server_config.qt_client_id](
                                           const contracts::mqtt::MqttMessage& message) {
        const auto* response = contracts::mqtt::GetPayload<contracts::mqtt::CommandResponsePayload>(message);
        if (response == nullptr || !contracts::mqtt::IsTerminal(response->result)) {
            return true;
        }
        const auto pending = pending_system_commands.find(response->request_id);
        if (pending == pending_system_commands.end()) {
            return true;
        }
        const auto command = pending->second;
        const bool succeeded = response->result == contracts::mqtt::CommandResult::kSuccess ||
                               response->result == contracts::mqtt::CommandResult::kDuplicated;
        ProcessTransition transition{
            .disposition = TransitionDisposition::kDuplicate,
            .previous_stage = std::nullopt,
            .current_stage = std::nullopt,
            .reason = "system command result does not change the process state",
        };
        if (succeeded) {
            if (command == contracts::mqtt::ControlCommand::kRecovery) {
                return false;
            }
            pending_system_commands.erase(pending);
        } else {
            pending_system_commands.erase(pending);
            transition = process_orchestrator.FailSystemCommand(
                command, response->result, response->message.empty() ? "system command failed" : response->message);
        }
        if (transition.disposition == TransitionDisposition::kRejected) {
            std::cerr << "[server][ERROR] system command result transition failed: " << transition.reason << '\n';
            return false;
        }
        const bool resumes_unassigned_work = succeeded &&
                                             (command == contracts::mqtt::ControlCommand::kStart ||
                                              command == contracts::mqtt::ControlCommand::kRestart) &&
                                             !restored_input_detected_work_ids.empty();
        if (transition.Applied() && !resumes_unassigned_work && !persist_process_state()) {
            return false;
        }
        if (succeeded && (command == contracts::mqtt::ControlCommand::kStart ||
                          command == contracts::mqtt::ControlCommand::kRestart)) {
            std::vector<PendingMqttDelivery> restored_deliveries;
            for (const auto& work_id : restored_input_detected_work_ids) {
                const auto assignment = process_orchestrator.ConfirmVisionAssignment("WORK-" + work_id, work_id);
                if (!assignment.Applied()) {
                    std::cerr << "[server][ERROR] restored vision assignment failed: " << work_id << '\n';
                    return false;
                }
                const auto work = process_orchestrator.StateMachine().FindWork(work_id);
                if (!work.has_value()) {
                    return false;
                }
                const contracts::mqtt::MqttMessage created{
                    .protocol_version = std::string(contracts::mqtt::kCurrentProtocolVersion),
                    .message_id = "WORK-" + work_id,
                    .message_type = contracts::mqtt::MessageType::kWorkCreated,
                    .source_id = "central-server",
                    .timestamp = CurrentIso8601Timestamp(),
                    .process_epoch = process_epoch,
                    .data = contracts::mqtt::WorkCreatedPayload{ .work_id = work_id },
                };
                restored_deliveries.push_back(
                    { .topic = contracts::mqtt::DeviceCommandTopic(process_orchestrator.VisionDeviceId()),
                      .message = created });
                restored_deliveries.push_back(
                    { .topic = contracts::mqtt::QtEventTopic(qt_client_id), .message = created });
            }
            if (!restored_deliveries.empty() &&
                !run_runtime_store(
                    [&] {
                        return process_state_store.Save(
                            process_orchestrator.StateMachine().SystemState(), process_orchestrator.MessageSequence(),
                            process_orchestrator.StateMachine().ActiveWorks(), process_orchestrator.GripperTargets(),
                            process_command_tracker.PendingCommands(), CurrentUnixTimeMilliseconds(),
                            restored_deliveries, process_orchestrator.StateMachine().ProcessedMessageIds(),
                            command_manager.Snapshot(), pending_system_commands, process_epoch);
                    },
                    "restored vision state/outbox persistence")) {
                return false;
            }
            // A VisionAssigned snapshot already has its durable WORK_CREATED row.
            // START only needs to release that row; it must not remain held just
            // because no InputDetected assignment had to be rebuilt.
            restored_input_detected_work_ids.clear();
            restored_vision_assigned_work_ids.clear();
            for (const auto& delivery : restored_deliveries) {
                static_cast<void>(publish_enqueued(delivery));
            }
        }
        return !succeeded ||
               (command != contracts::mqtt::ControlCommand::kStart &&
                command != contracts::mqtt::ControlCommand::kRestart) ||
               !replay_restored_process_commands || replay_restored_process_commands(false);
    };
    mqtt_handler.SetQtResponseHandler([&command_manager, &process_orchestrator, &commit_recovery_response,
                                       &finish_system_command, &process_command_tracker, &persist_process_state,
                                       &publish_qt_response, &dispatch_process_commands,
                                       &pending_system_commands](const contracts::mqtt::MqttMessage& message) {
        const auto decision = command_manager.PreviewResponse(message);
        switch (decision.disposition) {
            case CommandResponseDisposition::kForward: {
                if (!decision.message.has_value()) {
                    return false;
                }
                const auto* response =
                    contracts::mqtt::GetPayload<contracts::mqtt::CommandResponsePayload>(*decision.message);
                if (response != nullptr && response->command == contracts::mqtt::ControlCommand::kRecovery &&
                    (response->result == contracts::mqtt::CommandResult::kSuccess ||
                     response->result == contracts::mqtt::CommandResult::kDuplicated) &&
                    pending_system_commands.contains(response->request_id)) {
                    return commit_recovery_response(message);
                }
                // The terminal response is the durable commit point.  Do not consume the
                // aggregate before it is recoverable in the MQTT outbox.
                if (!publish_qt_response(*decision.message)) {
                    return false;
                }
                const auto committed = command_manager.HandleResponse(message);
                if (committed.disposition != CommandResponseDisposition::kForward) {
                    return committed.disposition == CommandResponseDisposition::kDuplicate;
                }
                if (const auto completed_intent = process_command_tracker.HandleResponse(message)) {
                    const bool succeeded =
                        response != nullptr && (response->result == contracts::mqtt::CommandResult::kSuccess ||
                                                response->result == contracts::mqtt::CommandResult::kDuplicated);
                    if (succeeded) {
                        const auto completion =
                            process_orchestrator.HandleCommandCompletion(*completed_intent, message);
                        if (completion.handled &&
                            (completion.transition.disposition == TransitionDisposition::kRejected ||
                             !dispatch_process_commands(completion.commands))) {
                            std::cerr << "[server][ERROR] process command completion could not be dispatched\n";
                            return false;
                        }
                    } else {
                        const auto transition = process_orchestrator.FailCommandResponse(
                            *completed_intent,
                            response == nullptr ? contracts::mqtt::CommandResult::kFailed : response->result,
                            response == nullptr ? "process command failed" : response->message);
                        if (transition.disposition == TransitionDisposition::kRejected) {
                            std::cerr << "[server][ERROR] process command failure transition rejected: "
                                      << transition.reason << '\n';
                            return false;
                        }
                    }
                }
                if (!finish_system_command(*decision.message)) {
                    return false;
                }
                return persist_process_state();
            }
            case CommandResponseDisposition::kDuplicate:
                std::clog << "[server][INFO] duplicate command response ignored: " << decision.reason << '\n';
                return true;
            case CommandResponseDisposition::kLateResponse:
                std::clog << "[server][INFO] LATE_RESPONSE recorded without process transition: " << decision.reason
                          << '\n';
                return true;
            case CommandResponseDisposition::kUnknownRequest:
                std::clog << "[server][INFO] unknown command response ignored: " << decision.reason << '\n';
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
        [&publish_durable, qt_client_id = server_config.qt_client_id](const contracts::mqtt::MqttMessage& message) {
            return publish_durable(contracts::mqtt::QtErrorTopic(qt_client_id), message);
        });
    mqtt_handler.SetProcessMessageGuard([&process_orchestrator](const contracts::mqtt::MqttMessage& message) {
        const auto preview = process_orchestrator.Preview(message);
        if (!preview.handled || preview.transition.disposition != TransitionDisposition::kRejected) {
            return true;
        }
        std::cerr << "[server][ERROR] invalid process transition: " << preview.transition.reason << '\n';
        return false;
    });

    const auto dispatch_command = [&mqtt_handler, &device_manager, &command_manager, &process_orchestrator,
                                   &server_config, &pending_system_commands, &finish_system_command,
                                   &process_state_persistence_healthy, &persist_process_state, &publish_durable,
                                   &publish_qt_response, &restored_input_detected_work_ids,
                                   &restored_vision_assigned_work_ids,
                                   &input_detection_gate](const contracts::mqtt::MqttMessage& message) {
        if (!process_state_persistence_healthy.load()) {
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
        const auto commit_system_command = [&process_orchestrator, &system_command, &restored_input_detected_work_ids,
                                            &restored_vision_assigned_work_ids, &input_detection_gate]() {
            if (!system_command.has_value()) {
                return true;
            }
            // Keep same-runtime suspended assignments as well as startup-restored ones.
            // START uses these sets to reissue/retain WORK_CREATED exactly once.
            if (*system_command == contracts::mqtt::ControlCommand::kStop ||
                *system_command == contracts::mqtt::ControlCommand::kEmergencyStop) {
                for (const auto& work : process_orchestrator.StateMachine().ActiveWorks()) {
                    if (work.stage == WorkStage::kInputDetected) {
                        restored_input_detected_work_ids.insert(work.work_id);
                    } else if (work.stage == WorkStage::kVisionAssigned) {
                        restored_vision_assigned_work_ids.insert(work.work_id);
                    }
                }
            }
            if (*system_command == contracts::mqtt::ControlCommand::kStart ||
                *system_command == contracts::mqtt::ControlCommand::kRestart ||
                *system_command == contracts::mqtt::ControlCommand::kStop ||
                *system_command == contracts::mqtt::ControlCommand::kRecovery) {
                input_detection_gate.RequireClear();
            }
            const auto transition = process_orchestrator.ApplySystemCommand(*system_command);
            if (transition.disposition == TransitionDisposition::kRejected) {
                std::cerr << "[server][ERROR] system process command commit failed: " << transition.reason << '\n';
                return false;
            }
            return true;
        };

        auto route = ResolveCommandTargets(message, device_manager->RegisteredDevices());
        if (!server_config.process.line_tracer_enabled) {
            std::erase(route.target_device_ids, server_config.process.line_tracer_device_id);
        }
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
        if (!commit_system_command()) {
            return false;
        }
        if (system_command.has_value()) {
            pending_system_commands.insert_or_assign(request_id, *system_command);
        }
        if (!persist_process_state()) {
            return false;
        }

        if (route.broadcast) {
            auto forwarded = message;
            auto* payload = contracts::mqtt::GetPayload<contracts::mqtt::EmergencyStopPayload>(forwarded);
            if (payload == nullptr) {
                return false;
            }
            payload->target_device_id = "ALL";
            if (publish_durable(contracts::mqtt::kSystemBroadcastCommandTopic, forwarded)) {
                return true;
            }
            const auto failed =
                command_manager.HandleDispatchFailures(request_id, route.target_device_ids, CurrentIso8601Timestamp());
            if (failed.has_value()) {
                static_cast<void>(finish_system_command(*failed) && publish_qt_response(*failed));
            }
            return false;
        }

        const auto publish_to_device = [&publish_durable, &message, &server_config](std::string_view device_id) {
            const auto forwarded =
                PrepareCommandForDevice(message, device_id, server_config.process.line_tracer_device_id,
                                        server_config.process.line_tracer_initial_position);
            return publish_durable(contracts::mqtt::DeviceCommandTopic(device_id), forwarded);
        };

        std::vector<std::string> failed_devices;
        for (const auto& device_id : route.target_device_ids) {
            if (!publish_to_device(device_id)) {
                failed_devices.push_back(device_id);
            }
        }
        if (failed_devices.empty()) {
            return true;
        }
        const auto failure =
            command_manager.HandleDispatchFailures(request_id, failed_devices, CurrentIso8601Timestamp());
        if (failure.has_value()) {
            static_cast<void>(finish_system_command(*failure) && publish_qt_response(*failure));
        }
        return false;
    };
    const auto fail_process_dispatch = [&command_manager, &process_orchestrator, &persist_process_state,
                                        &publish_failed_work, &publish_qt_response](const ProcessCommandIntent& intent,
                                                                                    std::string_view reason) {
        const auto transition = process_orchestrator.FailDispatch(intent, std::string(reason));
        if (!transition.Applied() || !persist_process_state()) {
            return false;
        }
        if (!publish_failed_work(intent.message.message_id + "-WORK-FAILED", intent.work_id, reason)) {
            return false;
        }
        const auto response = command_manager.MakeImmediateResult(
            intent.message, contracts::mqtt::CommandResult::kFailed, CurrentIso8601Timestamp(),
            std::string("ERR-PROCESS-DISPATCH"), std::string(reason));
        return response.has_value() && publish_qt_response(*response);
    };
    dispatch_process_commands = [&device_manager, &dispatch_command, &process_orchestrator, &process_command_tracker,
                                 &persist_process_state,
                                 &fail_process_dispatch](const std::vector<ProcessCommandIntent>& commands) {
        for (const auto& intent : commands) {
            const std::string request_log_id = ProcessCommandRequestId(intent.message).value_or("<none>");
            std::clog << "[server][PROCESS][INFO] dispatch requested; workId=" << intent.work_id
                      << "; messageId=" << intent.message.message_id << "; requestId=" << request_log_id;
            if (const auto* command =
                    contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(intent.message);
                command != nullptr) {
                std::clog << "; command=" << contracts::mqtt::ToString(command->command)
                          << "; target=" << command->target_device_id << "; component=" << command->component_id;
            }
            std::clog << '\n';
            if (!ResolveCommandTargets(intent.message, device_manager->RegisteredDevices()).IsValid()) {
                if (!fail_process_dispatch(intent, "target process node is unavailable")) {
                    std::cerr << "[server][ERROR] process command failure could not be reported: " << intent.work_id
                              << '\n';
                }
                std::cerr << "[server][ERROR] process command target is unavailable: " << intent.work_id << '\n';
                return false;
            }
            const auto request_id = ProcessCommandRequestId(intent.message);
            if (!request_id.has_value() || !process_command_tracker.Track(intent)) {
                static_cast<void>(process_orchestrator.FailDispatch(intent, "process command tracking failed"));
                static_cast<void>(persist_process_state());
                return false;
            }
            if (!persist_process_state()) {
                static_cast<void>(process_command_tracker.Remove(*request_id));
                return false;
            }
            if (!dispatch_command(intent.message)) {
                static_cast<void>(process_command_tracker.Remove(*request_id));
                if (!fail_process_dispatch(intent, "process command publication failed")) {
                    std::cerr << "[server][ERROR] process command publication failure could not be reported: "
                              << intent.work_id << '\n';
                }
                return false;
            }
            std::clog << "[server][PROCESS][INFO] dispatch accepted; workId=" << intent.work_id
                      << "; messageId=" << intent.message.message_id << "; requestId=" << *request_id << '\n';
            const auto confirmed = process_orchestrator.ConfirmDispatch(intent);
            if (!confirmed.Applied()) {
                static_cast<void>(process_command_tracker.Remove(*request_id));
                static_cast<void>(process_orchestrator.FailDispatch(intent, "process dispatch transition rejected"));
                static_cast<void>(persist_process_state());
                std::cerr << "[server][ERROR] process dispatch transition rejected: " << confirmed.reason << '\n';
                return false;
            }
            if (!process_command_tracker.MarkDispatched(*request_id)) {
                static_cast<void>(process_command_tracker.Remove(*request_id));
                static_cast<void>(process_orchestrator.FailDispatch(intent, "process outbox update failed"));
                static_cast<void>(persist_process_state());
                return false;
            }
            if (!persist_process_state()) {
                static_cast<void>(process_command_tracker.Remove(*request_id));
                static_cast<void>(process_orchestrator.FailDispatch(intent, "process state persistence failed"));
                static_cast<void>(persist_process_state());
                return false;
            }
        }
        return true;
    };
    replay_restored_process_commands = [&dispatch_command, &process_command_tracker, &process_orchestrator,
                                        &restored_pending_process_commands, &persist_process_state](bool stops_only) {
        for (const auto& intent : process_command_tracker.PendingCommands()) {
            const auto request_id = ProcessCommandRequestId(intent.message);
            if (!request_id.has_value() || !restored_pending_process_commands.contains(*request_id)) {
                continue;
            }
            const auto* control = contracts::mqtt::GetPayload<contracts::mqtt::ControlCommandPayload>(intent.message);
            if (stops_only && (control == nullptr || control->command != contracts::mqtt::ControlCommand::kStop)) {
                continue;
            }
            if (!dispatch_command(intent.message)) {
                static_cast<void>(process_command_tracker.Remove(*request_id));
                restored_pending_process_commands.erase(*request_id);
                static_cast<void>(process_orchestrator.FailDispatch(intent, "restored process command failed"));
                static_cast<void>(persist_process_state());
                return false;
            }
            if (!intent.dispatch_confirmed) {
                const auto confirmed = process_orchestrator.ConfirmDispatch(intent);
                if (!confirmed.Applied() || !process_command_tracker.MarkDispatched(*request_id) ||
                    !persist_process_state()) {
                    static_cast<void>(process_command_tracker.Remove(*request_id));
                    restored_pending_process_commands.erase(*request_id);
                    static_cast<void>(process_orchestrator.FailDispatch(
                        intent, "restored process command state could not be confirmed"));
                    static_cast<void>(persist_process_state());
                    return false;
                }
            }
            restored_pending_process_commands.erase(*request_id);
        }
        return true;
    };
    mqtt_handler.SetCommandRouteHandler(dispatch_command);
    const auto replay_pending_vision_measurement = [&]() {
        const auto active_works = process_orchestrator.StateMachine().ActiveWorks();
        const bool vision_work_ready = std::ranges::any_of(active_works, [](const WorkProcessSnapshot& work) {
            return work.stage == WorkStage::kVisionAssigned || work.stage == WorkStage::kVisionProcessing;
        });
        std::string message_id;
        bool replayed = false;
        const bool handled =
            pending_vision_measurement.ReplayWhen(vision_work_ready, [&](const contracts::mqtt::MqttMessage& pending) {
                replayed = true;
                message_id = pending.message_id;
                const auto encoded = contracts::mqtt::SerializeMessage(pending);
                return encoded.IsSuccess() && mqtt_handler.Handle(contracts::mqtt::DeviceEventTopic(pending.source_id),
                                                                  encoded.payload, {}, 1, false);
            });
        if (!handled) {
            std::cerr << "[server][WARN] buffered VISION_MEASUREMENT replay deferred; messageId=" << message_id << '\n';
            return false;
        }
        if (replayed) {
            std::clog << "[server][INFO] buffered VISION_MEASUREMENT replayed; messageId=" << message_id << '\n';
        }
        return true;
    };
    mqtt_handler.SetProcessMessageHandler([&mqtt_handler, &process_orchestrator, &dispatch_command,
                                           &dispatch_process_commands, &input_detection_gate, &line_tracer_load_gate,
                                           &process_state_persistence_healthy, &persist_process_state, &persistence,
                                           &pending_vision_measurement, &publish_durable,
                                           &replay_pending_vision_measurement,
                                           qt_client_id = server_config.qt_client_id,
                                           default_destination = server_config.process.default_destination,
                                           line_tracer_enabled = server_config.process.line_tracer_enabled,
                                           &process_epoch](const contracts::mqtt::MqttMessage& message) {
        if (!process_state_persistence_healthy.load()) {
            return false;
        }

        if (message.message_type == contracts::mqtt::MessageType::kVisionMeasurement) {
            const auto* measurement = contracts::mqtt::GetPayload<contracts::mqtt::VisionMeasurementPayload>(message);
            if (measurement == nullptr || !measurement->IsValid()) {
                std::cerr << "[server][ERROR] invalid VISION_MEASUREMENT payload\n";
                return false;
            }
            if (message.source_id != process_orchestrator.VisionDeviceId()) {
                std::clog << "[server][INFO] VISION_MEASUREMENT ignored; non-authoritative source=" << message.source_id
                          << '\n';
                return true;
            }
            const std::string position_summary =
                std::to_string(measurement->box_x) + "," + std::to_string(measurement->box_y) + "," +
                std::to_string(measurement->box_width) + "," + std::to_string(measurement->box_height);

            const auto active_works = process_orchestrator.StateMachine().ActiveWorks();
            const auto work_it = std::ranges::find_if(active_works, [](const WorkProcessSnapshot& work) {
                return work.stage == WorkStage::kVisionAssigned || work.stage == WorkStage::kVisionProcessing;
            });
            if (work_it == active_works.end()) {
                pending_vision_measurement.Store(message);
                std::clog << "[server][INFO] VISION_MEASUREMENT buffered; no ultrasonic work; source="
                          << message.source_id << "; barcode=" << measurement->barcode
                          << "; position=" << position_summary << '\n';
                return true;
            }

            const auto& work = *work_it;
            const std::string position_id = "VISION-POSITION-" + message.message_id;
            const std::string barcode_id = "VISION-BARCODE-" + message.message_id;
            const std::int32_t center_x = measurement->box_x + measurement->box_width / 2;
            const std::int32_t center_y = measurement->box_y + measurement->box_height / 2;
            const std::int32_t frame_center_x = measurement->frame_width / 2;
            const std::int32_t frame_center_y = measurement->frame_height / 2;
            const contracts::mqtt::MqttMessage position_message{
                .protocol_version = message.protocol_version,
                .message_id = position_id,
                .message_type = contracts::mqtt::MessageType::kPositionDetected,
                .source_id = message.source_id,
                .timestamp = message.timestamp,
                .process_epoch = process_epoch,
                .data =
                    contracts::mqtt::PositionDetectedPayload{
                        .work_id = work.work_id,
                        .box_x = measurement->box_x,
                        .box_y = measurement->box_y,
                        .box_width = measurement->box_width,
                        .box_height = measurement->box_height,
                        .center_x = center_x,
                        .center_y = center_y,
                        .offset_x = center_x - frame_center_x,
                        .offset_y = center_y - frame_center_y,
                        .position_status = "DETECTED",
                        .box_corners = measurement->box_corners,
                    },
            };
            const contracts::mqtt::MqttMessage barcode_message{
                .protocol_version = message.protocol_version,
                .message_id = barcode_id,
                .message_type = contracts::mqtt::MessageType::kBarcodeDetected,
                .source_id = message.source_id,
                .timestamp = message.timestamp,
                .process_epoch = process_epoch,
                .data =
                    contracts::mqtt::BarcodeDetectedPayload{
                        .work_id = work.work_id,
                        .recognition_status = "SUCCESS",
                        .barcode = measurement->barcode,
                        .confidence = std::nullopt,
                        .message = std::nullopt,
                        .error_code = std::nullopt,
                        .failure_stage = std::nullopt,
                    },
            };

            const auto position_result = process_orchestrator.Handle(position_message);
            if (position_result.transition.disposition == TransitionDisposition::kRejected) {
                std::clog << "[server][INFO] VISION_MEASUREMENT ignored; position already processed; workId="
                          << work.work_id << '\n';
                return true;
            }
            const auto barcode_result = process_orchestrator.Handle(barcode_message);
            if (barcode_result.transition.disposition == TransitionDisposition::kRejected) {
                std::cerr << "[server][ERROR] VISION_MEASUREMENT barcode transition rejected; workId=" << work.work_id
                          << "; reason=" << barcode_result.transition.reason << '\n';
                return false;
            }

            const auto publish_measurement_event = [&publish_durable,
                                                    qt_client_id](const contracts::mqtt::MqttMessage& event) {
                if (!publish_durable(contracts::mqtt::QtEventTopic(qt_client_id), event)) {
                    std::cerr << "[server][ERROR] VISION_MEASUREMENT Qt event enqueue failed; messageId="
                              << event.message_id << '\n';
                }
            };
            publish_measurement_event(position_message);
            publish_measurement_event(barcode_message);

            std::optional<CatalogProduct> catalog_product;
            const auto lookup_status = persistence.FindActiveProductByBarcode(measurement->barcode, catalog_product);
            if (!lookup_status.ok()) {
                std::cerr << "[server][ERROR] vision barcode catalog lookup failed: " << lookup_status.message << '\n';
                return false;
            }
            if (!catalog_product && !default_destination.empty()) {
                catalog_product = CatalogProduct{
                    .barcode = measurement->barcode,
                    .product_id = "UNREGISTERED",
                    .product_name = "Unregistered product",
                    .destination = default_destination,
                };
            }
            if (!catalog_product) {
                std::clog << "[server][INFO] VISION_MEASUREMENT accepted; product catalog pending; workId="
                          << work.work_id << "; barcode=" << measurement->barcode << '\n';
                return persist_process_state();
            }

            const contracts::mqtt::MqttMessage product_message{
                .protocol_version = message.protocol_version,
                .message_id = "VISION-PRODUCT-" + message.message_id,
                .message_type = contracts::mqtt::MessageType::kProductInfo,
                .source_id = "central-server",
                .timestamp = message.timestamp,
                .process_epoch = process_epoch,
                .data =
                    contracts::mqtt::ProductInfoPayload{
                        .work_id = work.work_id,
                        .recognition_status = "SUCCESS",
                        .barcode = catalog_product->barcode,
                        .product_id = catalog_product->product_id,
                        .product_name = catalog_product->product_name,
                        .destination = catalog_product->destination,
                        .image = nullptr,
                        .confidence = std::nullopt,
                        .message = std::nullopt,
                    },
            };
            const auto product_result = process_orchestrator.Handle(product_message);
            if (product_result.transition.disposition == TransitionDisposition::kRejected) {
                std::clog << "[server][INFO] VISION_MEASUREMENT product already processed; workId=" << work.work_id
                          << '\n';
                return true;
            }
            publish_measurement_event(product_message);
            std::clog << "[server][INFO] VISION_MEASUREMENT accepted; workId=" << work.work_id
                      << "; barcode=" << measurement->barcode << "; position=" << position_summary
                      << "; destination=" << catalog_product->destination << "; state=PRODUCT_IDENTIFIED\n";
            if (!product_result.commands.empty()) {
                return dispatch_process_commands(product_result.commands);
            }
            return persist_process_state();
        }

        const auto system_state = process_orchestrator.StateMachine().SystemState();
        if (line_tracer_enabled) {
            if (const auto work_id =
                    line_tracer_load_gate.ShouldStop(message, system_state == ProcessSystemState::kRunning,
                                                     process_orchestrator.StateMachine().ActiveWorks())) {
                const auto commands = process_orchestrator.SortingDetectionCommands(*work_id, message.timestamp);
                if (commands.empty() || !dispatch_process_commands(commands)) {
                    line_tracer_load_gate.Retry(*work_id);
                    return false;
                }
            }
        }
        const bool process_accepts_work = process_orchestrator.AcceptsInputWorkCreation();
        const bool has_active_work = !process_orchestrator.StateMachine().ActiveWorks().empty();
        const bool input_sensor_detected = input_detection_gate.ShouldStopConveyor(message);
        if (input_sensor_detected) {
            const auto* sensor = contracts::mqtt::GetPayload<contracts::mqtt::SensorStatusPayload>(message);
            std::clog << "[server][SENSOR][INFO] input stop decision; messageId=" << message.message_id
                      << "; processState=" << ToString(system_state)
                      << "; processAcceptsWork=" << (process_accepts_work ? "true" : "false") << "; stationOccupied="
                      << (InputStationOccupied(process_orchestrator.StateMachine()) ? "true" : "false");
            if (sensor != nullptr) {
                std::clog << "; sensorId=" << sensor->sensor_id << "; distanceCm=" << sensor->distance_cm;
            }
            std::clog << '\n';
        }
        if (process_orchestrator.Enabled() && !process_accepts_work && input_sensor_detected) {
            const auto stop = process_orchestrator.MakeInputConveyorSafetyStop(message.message_id, message.timestamp);
            if (!dispatch_command(stop)) {
                if (input_sensor_detected) {
                    input_detection_gate.RetryStop();
                }
                return false;
            }
        }
        const bool create_work = input_detection_gate.ShouldCreateWork(
            message, process_accepts_work, InputStationOccupied(process_orchestrator.StateMachine()), has_active_work);
        if (create_work) {
            const auto* sensor = contracts::mqtt::GetPayload<contracts::mqtt::SensorStatusPayload>(message);
            std::clog << "[server][PROCESS][INFO] input sensor created work; messageId=" << message.message_id
                      << "; source=" << message.source_id
                      << "; sensorId=" << (sensor != nullptr ? std::to_string(sensor->sensor_id) : "<unknown>") << '\n';
            const contracts::mqtt::MqttMessage box_detected{
                .protocol_version = message.protocol_version,
                .message_id = "SENSOR-BOX-" + message.message_id,
                .message_type = contracts::mqtt::MessageType::kBoxDetected,
                .source_id = message.source_id,
                .timestamp = message.timestamp,
                .process_epoch = process_epoch,
                .data =
                    contracts::mqtt::BoxDetectedPayload{
                        .detected = true,
                        .image_name = "ultrasonic-sensor-" + std::to_string(sensor->sensor_id),
                    },
            };
            const auto encoded = contracts::mqtt::SerializeMessage(box_detected);
            if (!encoded.IsSuccess() || !mqtt_handler.Handle(contracts::mqtt::DeviceEventTopic(message.source_id),
                                                             encoded.payload, {}, 1, false)) {
                input_detection_gate.Retry();
                return false;
            }
            static_cast<void>(replay_pending_vision_measurement());
        }
        const auto result = process_orchestrator.Handle(message);
        if (!result.handled) {
            return true;
        }
        if (result.transition.disposition == TransitionDisposition::kRejected) {
            std::cerr << "[server][ERROR] process transition commit rejected: " << result.transition.reason << '\n';
            return false;
        }
        // A transition that emits commands is persisted by dispatch_process_commands together with
        // its command tracker entry.  Saving it first leaves a restart window with no replayable
        // command, so only command-free transitions are saved here.
        return result.commands.empty() ? persist_process_state() : dispatch_process_commands(result.commands);
    });

    mqtt_client.SetMessageHandler([&mqtt_handler, &process_orchestrator, &persist_process_state,
                                   &process_state_persistence_healthy, &process_mutex](
                                      std::string_view topic, std::string_view payload, int qos, bool retained) {
        const std::lock_guard process_lock(process_mutex);
        if (!process_state_persistence_healthy.load()) {
            return;
        }
        const auto previous_revision = process_orchestrator.Revision();
        if (!mqtt_handler.Handle(topic, payload, {}, qos, retained)) {
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
    {
        const std::lock_guard process_lock(process_mutex);
        static_cast<void>(pump_durable());
    }
    const auto publish_recalibration_notifications = [&work_invalidations, &publish_durable, &server_config]() {
        bool published = true;
        for (const auto& invalidation : work_invalidations) {
            const auto error = MakeWorkInvalidationError("central-server", invalidation, CurrentIso8601Timestamp());
            if (!publish_durable(contracts::mqtt::QtErrorTopic(server_config.qt_client_id), error)) {
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
        {
            const std::lock_guard process_lock(process_mutex);
            static_cast<void>(pump_durable());
            static_cast<void>(replay_pending_vision_measurement());
            // A restored input work may have been persisted after its STOP tracker was
            // recorded but before WORK_CREATED was committed.  Restore that interlock
            // before replaying the BOX event, otherwise replay could assign vision first.
            if (safe_restored_replay_pending && mqtt_client.IsConnected()) {
                safe_restored_replay_pending = false;
                if (!replay_restored_process_commands(true)) {
                    std::cerr << "[server][ERROR] restored input STOP command replay failed\n";
                    safe_restored_replay_pending = true;
                    continue;
                }
            }
            if (mqtt_client.IsReady() && process_state_persistence_healthy.load() &&
                !mqtt_handler.ReplayPendingReceivedEvents()) {
                std::cerr << "[server][ERROR] pending MQTT event replay failed; retrying from the oldest event\n";
            }
        }
        if (loop_now >= next_retention_cleanup) {
            next_retention_cleanup = loop_now + std::chrono::hours(server_config.storage.cleanup_interval_hours);
            const auto retention_status = scheduled_retention.RunOnce(CurrentUnixTimeMilliseconds());
            if (!retention_status.ok()) {
                std::cerr << "[server][ERROR] scheduled retention cleanup failed: " << retention_status.message << '\n';
            }
        }
        if (recalibration_notifications_pending && mqtt_client.IsConnected()) {
            const std::lock_guard process_lock(process_mutex);
            if (process_state_persistence_healthy.load()) {
                recalibration_notifications_pending = !publish_recalibration_notifications();
            }
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
        {
            const std::lock_guard process_lock(process_mutex);
            if (!process_state_persistence_healthy.load()) {
                continue;
            }
            const auto checked_at = CurrentIso8601Timestamp();
            const auto pending_timeouts = command_manager.PreviewTimeouts(checked_at);
            bool all_timeout_responses_durable = true;
            for (const auto& timeout : pending_timeouts) {
                if (!publish_qt_response(timeout)) {
                    std::cerr << "[server][ERROR] command timeout publish failed\n";
                    all_timeout_responses_durable = false;
                    break;
                }
            }
            if (!all_timeout_responses_durable) {
                continue;
            }
            const auto consumed_timeouts = command_manager.CheckTimeouts(checked_at);
            if (consumed_timeouts.size() != pending_timeouts.size()) {
                std::cerr << "[server][ERROR] command timeout set changed before commit\n";
                continue;
            }
            for (const auto& timeout : consumed_timeouts) {
                if (const auto failed_intent = process_command_tracker.HandleResponse(timeout)) {
                    const auto* response =
                        contracts::mqtt::GetPayload<contracts::mqtt::CommandResponsePayload>(timeout);
                    static_cast<void>(process_orchestrator.FailCommandResponse(
                        *failed_intent,
                        response == nullptr ? contracts::mqtt::CommandResult::kTimeout : response->result,
                        response == nullptr ? "process command timed out" : response->message));
                }
                if (!finish_system_command(timeout)) {
                    std::cerr << "[server][ERROR] system command timeout transition failed\n";
                }
                if (!persist_process_state()) {
                    std::cerr << "[server][ERROR] command timeout state persistence failed\n";
                    break;
                }
            }
        }
    }

    upload_server.Stop();
    mqtt_client.Stop();

    bool shutdown_ok = process_state_persistence_healthy.load() && persist_process_state();
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
