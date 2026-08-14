#include "logistics/central_server/mqtt_handler.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logistics/central_server/device_manager.hpp"
#include "logistics/central_server/persistence.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_validation.hpp"
#include "logistics/contracts/uart/linetracer_commands.h"

#ifndef LOGISTICS_TEST_MIGRATION_DIR
#define LOGISTICS_TEST_MIGRATION_DIR "central-server-rpi/db/migrations"
#endif

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

struct LogEntry final {
    central_server::MqttHandlerLogLevel level;
    std::string message;
};

[[nodiscard]] mqtt::MqttMessage MakeRegistration(std::string source_id = "PI-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-REGISTER-01",
        .message_type = mqtt::MessageType::kDeviceRegister,
        .source_id = std::move(source_id),
        .timestamp = "2026-07-16T01:00:00Z",
        .data =
            mqtt::DeviceRegisterPayload{
                .device_type = "vision",
                .node_name = "vision-node-01",
                .status = mqtt::ConnectionState::kOnline,
                .ip_address = "192.168.0.21",
                .uart_connected = false,
            },
    };
}

[[nodiscard]] mqtt::MqttMessage MakePositionStatus(std::string source_id = "PI-01") {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-POSITION-STATUS-01",
        .message_type = mqtt::MessageType::kDeviceStatus,
        .source_id = std::move(source_id),
        .timestamp = "2026-07-16T01:00:03Z",
        .data =
            mqtt::DeviceStatusPayload{
                .status = mqtt::ConnectionState::kOnline,
                .current_state = "FOLLOWING_LINE",
                .job_id = std::string("WORK-103"),
                .error_code = std::nullopt,
                .departure_position = mqtt::LineTracerPositionPayload{ .area = "DEPARTURE", .location = "A" },
                .target_position = mqtt::LineTracerPositionPayload{ .area = "DESTINATION", .location = "C" },
                .confirmed_position = mqtt::LineTracerPositionPayload{ .area = "DEPARTURE", .location = "A" },
                .movement_state = std::string("MOVING"),
            },
    };
}

void AssertPositionStatus(const mqtt::DeviceStatusPayload& status) {
    assert(status.departure_position.has_value());
    assert(status.departure_position->area == "DEPARTURE");
    assert(status.departure_position->location == "A");
    assert(status.target_position.has_value());
    assert(status.target_position->area == "DESTINATION");
    assert(status.target_position->location == "C");
    assert(status.confirmed_position.has_value());
    assert(status.confirmed_position->area == "DEPARTURE");
    assert(status.confirmed_position->location == "A");
    assert(status.movement_state == std::optional<std::string>("MOVING"));
}

[[nodiscard]] std::string Encode(const mqtt::MqttMessage& message) {
    const auto encoded = mqtt::SerializeMessage(message);
    assert(encoded.IsSuccess());
    return encoded.payload;
}

void TestRegistrationIsDecodedValidatedAndRouted() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(device_manager,
                                        [&logs](central_server::MqttHandlerLogLevel level, std::string_view message) {
                                            logs.push_back({ .level = level, .message = std::string(message) });
                                        });

    assert(handler.Handle("device/PI-01/register", Encode(MakeRegistration()), "2026-07-16T01:00:01Z"));
    assert(device_manager.RegisteredDeviceCount() == 1);

    const auto device = device_manager.FindDevice("PI-01");
    assert(device.has_value());
    assert(device->registered);
    assert(device->device_type == "vision");
    assert(device->last_seen_timestamp == "2026-07-16T01:00:01Z");
    assert(logs.size() == 2);
    assert(logs.front().level == central_server::MqttHandlerLogLevel::kInfo);
    assert(logs.front().message == "MQTT message received: device/PI-01/register");
    assert(logs.back().message.find("registered devices=1") != std::string::npos);
}

void TestMalformedJsonIsRejected() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(device_manager,
                                        [&logs](central_server::MqttHandlerLogLevel level, std::string_view message) {
                                            logs.push_back({ .level = level, .message = std::string(message) });
                                        });

    assert(!handler.Handle("device/PI-01/register", "{"));
    assert(device_manager.RegisteredDeviceCount() == 0);
    assert(logs.size() == 1);
    assert(logs.front().level == central_server::MqttHandlerLogLevel::kError);
    assert(logs.front().message.find("invalid MQTT JSON") != std::string::npos);
}

void TestTopicMessageMismatchIsRejected() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(device_manager,
                                        [&logs](central_server::MqttHandlerLogLevel level, std::string_view message) {
                                            logs.push_back({ .level = level, .message = std::string(message) });
                                        });

    assert(!handler.Handle("device/PI-02/register", Encode(MakeRegistration("PI-01"))));
    assert(device_manager.RegisteredDeviceCount() == 0);
    assert(logs.size() == 1);
    assert(logs.front().level == central_server::MqttHandlerLogLevel::kError);
    assert(logs.front().message.find("SOURCE_ID_MISMATCH") != std::string::npos);
}

void TestUnsupportedVersionAndMissingFieldsAreRejected() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(device_manager,
                                        [&logs](central_server::MqttHandlerLogLevel level, std::string_view message) {
                                            logs.push_back({ .level = level, .message = std::string(message) });
                                        });

    auto unsupported_version = mqtt::Json::parse(Encode(MakeRegistration()));
    unsupported_version[std::string(mqtt::kProtocolVersionField)] = "2.0";
    assert(!handler.Handle("device/PI-01/register", unsupported_version.dump()));

    auto missing_data = mqtt::Json::parse(Encode(MakeRegistration()));
    missing_data.erase(std::string(mqtt::kDataField));
    assert(!handler.Handle("device/PI-01/register", missing_data.dump()));

    assert(device_manager.RegisteredDeviceCount() == 0);
    assert(logs.size() == 2);
    assert(logs[0].level == central_server::MqttHandlerLogLevel::kError);
    assert(logs[0].message.find("UNSUPPORTED_PROTOCOL_VERSION") != std::string::npos);
    assert(logs[1].level == central_server::MqttHandlerLogLevel::kError);
    assert(logs[1].message.find("MISSING_FIELD") != std::string::npos);
    assert(logs[1].message.find("field=data") != std::string::npos);
}

void TestUnknownMessageTypeLogIncludesReceivedTypeAndTopic() {
    central_server::DeviceManager device_manager;
    std::vector<LogEntry> logs;
    central_server::MqttHandler handler(
        device_manager, [&logs](const central_server::MqttHandlerLogLevel level, const std::string_view message) {
            logs.push_back({ level, std::string(message) });
        });

    auto unknown_type = mqtt::Json::parse(Encode(MakeRegistration()));
    unknown_type[std::string(mqtt::kMessageTypeField)] = "SENSOR_READING";
    assert(!handler.Handle("device/PI-01/event", unknown_type.dump()));
    assert(logs.size() == 1);
    assert(logs[0].message.find("UNKNOWN_MESSAGE_TYPE") != std::string::npos);
    assert(logs[0].message.find("receivedMessageType=SENSOR_READING") != std::string::npos);
    assert(logs[0].message.find("topic=device/PI-01/event") != std::string::npos);
}

void TestBarcodeUsesCatalogOrDefaultDestination() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("logistics-handler-test-" + unique);
    std::filesystem::create_directories(root);
    {
        central_server::Database database;
        const central_server::DatabaseConfig database_config{
            .path = root / "test.db",
            .migration_dir = LOGISTICS_TEST_MIGRATION_DIR,
            .busy_timeout_ms = 100,
        };
        assert(database.Open(database_config).ok());
        assert(central_server::MigrationRunner::Apply(database, database_config.migration_dir).ok());

        central_server::StorageConfig storage;
        storage.image_root = root / "images";
        central_server::PersistenceService persistence(database, storage);
        central_server::DeviceManager device_manager;
        central_server::MqttHandler handler(device_manager, {}, &persistence, "3");
        std::string work_id;
        std::vector<mqtt::MqttMessage> qt_events;
        handler.SetWorkCreatedHandler([&work_id](std::string_view, std::string_view created_work_id) {
            work_id = created_work_id;
            return true;
        });
        handler.SetQtEventHandler([&qt_events](const mqtt::MqttMessage& message) {
            qt_events.push_back(message);
            return true;
        });

        const mqtt::MqttMessage box{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-BOX-CATALOG",
            .message_type = mqtt::MessageType::kBoxDetected,
            .source_id = "PI-VISION-01",
            .timestamp = "2026-07-21T01:00:00Z",
            .data = mqtt::BoxDetectedPayload{ .detected = true, .image_name = "catalog-box.jpg" },
        };
        assert(handler.Handle("device/PI-VISION-01/event", Encode(box)));
        assert(!work_id.empty());

        const mqtt::MqttMessage barcode{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-BARCODE-CATALOG",
            .message_type = mqtt::MessageType::kBarcodeDetected,
            .source_id = "PI-VISION-01",
            .timestamp = "2026-07-21T01:00:01Z",
            .data =
                mqtt::BarcodeDetectedPayload{
                    .work_id = work_id,
                    .recognition_status = "SUCCESS",
                    .barcode = "5901234123457",
                    .confidence = 0.99,
                    .message = std::nullopt,
                },
        };
        assert(handler.Handle("device/PI-VISION-01/event", Encode(barcode)));
        assert(qt_events.size() == 2);
        assert(qt_events[0].message_type == mqtt::MessageType::kBarcodeDetected);
        const auto* product = mqtt::GetPayload<mqtt::ProductInfoPayload>(qt_events[1]);
        assert(product != nullptr);
        assert(product->work_id == work_id);
        assert(product->barcode == "5901234123457");
        assert(product->product_id == "VEDA107");
        assert(product->product_name == "VEDA107 기본 상품");
        assert(product->destination == "1");

        const mqtt::MqttMessage unknown_box{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-BOX-UNKNOWN",
            .message_type = mqtt::MessageType::kBoxDetected,
            .source_id = "PI-VISION-01",
            .timestamp = "2026-07-21T01:00:02Z",
            .data = mqtt::BoxDetectedPayload{ .detected = true, .image_name = "unknown-box.jpg" },
        };
        assert(handler.Handle("device/PI-VISION-01/event", Encode(unknown_box)));
        const mqtt::MqttMessage unknown_barcode{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-BARCODE-UNKNOWN",
            .message_type = mqtt::MessageType::kBarcodeDetected,
            .source_id = "PI-VISION-01",
            .timestamp = "2026-07-21T01:00:03Z",
            .data =
                mqtt::BarcodeDetectedPayload{
                    .work_id = work_id,
                    .recognition_status = "SUCCESS",
                    .barcode = "0000000000000",
                    .confidence = 0.95,
                    .message = std::nullopt,
                    .error_code = std::nullopt,
                    .failure_stage = std::nullopt,
                },
        };
        assert(handler.Handle("device/PI-VISION-01/event", Encode(unknown_barcode)));
        assert(qt_events.size() == 4);
        const auto* unknown_product = mqtt::GetPayload<mqtt::ProductInfoPayload>(qt_events.back());
        assert(unknown_product != nullptr);
        assert(unknown_product->product_id == "UNREGISTERED");
        assert(unknown_product->destination == "3");
    }
    std::filesystem::remove_all(root);
}

void TestHeartbeatIsForwardedToQtAsDeviceStatus() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("logistics-heartbeat-test-" + unique);
    std::filesystem::create_directories(root);
    {
        central_server::Database database;
        const central_server::DatabaseConfig database_config{
            .path = root / "test.db",
            .migration_dir = LOGISTICS_TEST_MIGRATION_DIR,
            .busy_timeout_ms = 100,
        };
        assert(database.Open(database_config).ok());
        assert(central_server::MigrationRunner::Apply(database, database_config.migration_dir).ok());

        central_server::StorageConfig storage;
        storage.image_root = root / "images";
        central_server::PersistenceService persistence(database, storage);
        central_server::DeviceManager device_manager;
        central_server::MqttHandler handler(device_manager, {}, &persistence);
        std::vector<mqtt::MqttMessage> qt_statuses;
        handler.SetQtStatusHandler([&qt_statuses](const mqtt::MqttMessage& message) {
            qt_statuses.push_back(message);
            return true;
        });

        assert(handler.Handle("device/PI-01/register", Encode(MakeRegistration()), "2026-07-16T01:00:01Z"));
        assert(handler.Handle("device/PI-01/status", Encode(MakePositionStatus()), "2026-07-16T01:00:03Z"));
        assert(qt_statuses.size() == 1);
        qt_statuses.clear();
        const mqtt::MqttMessage heartbeat{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-HEARTBEAT-QT-01",
            .message_type = mqtt::MessageType::kHeartbeat,
            .source_id = "PI-01",
            .timestamp = "2026-07-16T01:00:05Z",
            .data =
                mqtt::HeartbeatPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = "PICKING",
                    .uptime = 120,
                    .job_id = std::string("WORK-103"),
                    .error_code = std::nullopt,
                },
        };
        assert(handler.Handle("device/PI-01/heartbeat", Encode(heartbeat), "2026-07-16T01:00:05Z"));
        assert(qt_statuses.size() == 1);
        assert(qt_statuses[0].message_type == mqtt::MessageType::kDeviceStatus);
        assert(qt_statuses[0].source_id == "PI-01");
        const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(qt_statuses[0]);
        assert(status != nullptr);
        assert(status->status == mqtt::ConnectionState::kOnline);
        assert(status->current_state == "PICKING");
        assert(status->job_id == std::optional<std::string>("WORK-103"));
        AssertPositionStatus(*status);
        assert(mqtt::ValidateTopicMessage(mqtt::QtStatusTopic("control-center"), qt_statuses[0]).IsSuccess());
    }
    std::filesystem::remove_all(root);
}

void TestSensorStatusIsAcceptedAndForwardedToQt() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("logistics-sensor-status-test-" + unique);
    std::filesystem::create_directories(root);
    {
        central_server::Database database;
        const central_server::DatabaseConfig database_config{
            .path = root / "test.db",
            .migration_dir = LOGISTICS_TEST_MIGRATION_DIR,
            .busy_timeout_ms = 100,
        };
        assert(database.Open(database_config).ok());
        assert(central_server::MigrationRunner::Apply(database, database_config.migration_dir).ok());

        central_server::StorageConfig storage;
        storage.image_root = root / "images";
        central_server::PersistenceService persistence(database, storage);
        central_server::DeviceManager device_manager;
        // debounce_count=1 keeps this test about routing; the debounce itself is
        // covered by sensor_detection_test.
        const central_server::SensorDetectionConfig detection{
            .enabled = true,
            .enter_threshold_cm = 10,
            .exit_threshold_cm = 12,
            .debounce_count = 1,
        };
        central_server::MqttHandler handler(device_manager, {}, &persistence, {}, detection);
        std::vector<mqtt::MqttMessage> qt_events;
        handler.SetQtEventHandler([&qt_events](const mqtt::MqttMessage& message) {
            qt_events.push_back(message);
            return true;
        });

        auto registration = MakeRegistration("PI-LINETRACER-01");
        auto* registration_payload = mqtt::GetPayload<mqtt::DeviceRegisterPayload>(registration);
        assert(registration_payload != nullptr);
        registration_payload->device_type = "linetracer";
        registration_payload->node_name = "linetracer-node-01";
        assert(handler.Handle(mqtt::DeviceRegisterTopic("PI-LINETRACER-01"), Encode(registration)));

        // The device reports measurement health and a distance; it never sets
        // detectionStatus. The handler must stamp that in on the way through.
        const mqtt::MqttMessage sensor_status{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-SENSOR-STATUS-01",
            .message_type = mqtt::MessageType::kSensorStatus,
            .source_id = "PI-LINETRACER-01",
            .timestamp = "2026-07-27T02:00:00Z",
            .data =
                mqtt::SensorStatusPayload{
                    .sensor_id = 4,
                    .measurement_status = "OK",
                    .distance_cm = 8,
                    .detection_status = std::nullopt,
                },
        };

        auto retired_rear = sensor_status;
        retired_rear.message_id = "MSG-SENSOR-STATUS-RETIRED-REAR";
        auto* retired_rear_payload = mqtt::GetPayload<mqtt::SensorStatusPayload>(retired_rear);
        assert(retired_rear_payload != nullptr);
        retired_rear_payload->sensor_id = UART_LINETRACER_RETIRED_REAR_SENSOR_ID;
        assert(handler.Handle(mqtt::DeviceEventTopic("PI-LINETRACER-01"), Encode(retired_rear)));
        assert(qt_events.empty());

        assert(handler.Handle(mqtt::DeviceEventTopic("PI-LINETRACER-01"), Encode(sensor_status)));
        assert(qt_events.size() == 1);
        const auto* forwarded = mqtt::GetPayload<mqtt::SensorStatusPayload>(qt_events.front());
        assert(forwarded != nullptr);
        assert(forwarded->sensor_id == 4);
        assert(forwarded->measurement_status == "OK");
        assert(forwarded->distance_cm == 8);
        assert(forwarded->detection_status.has_value() && *forwarded->detection_status == "DETECTED");

        auto sensor_clear = sensor_status;
        sensor_clear.message_id = "MSG-SENSOR-STATUS-02";
        sensor_clear.timestamp = "2026-07-27T02:00:01Z";
        auto* clear_payload = mqtt::GetPayload<mqtt::SensorStatusPayload>(sensor_clear);
        assert(clear_payload != nullptr);
        clear_payload->distance_cm = 20;
        assert(handler.Handle(mqtt::DeviceEventTopic("PI-LINETRACER-01"), Encode(sensor_clear)));
        assert(qt_events.size() == 2);
        forwarded = mqtt::GetPayload<mqtt::SensorStatusPayload>(qt_events.back());
        assert(forwarded != nullptr);
        assert(forwarded->sensor_id == 4);
        assert(forwarded->measurement_status == "OK");
        assert(forwarded->distance_cm == 20);
        assert(forwarded->detection_status.has_value() && *forwarded->detection_status == "CLEAR");

        // A faulty sensor cannot be reasoned about, so detection reports UNKNOWN
        // rather than leaving the last CLEAR/DETECTED standing.
        auto sensor_fault = sensor_status;
        sensor_fault.message_id = "MSG-SENSOR-STATUS-03";
        sensor_fault.timestamp = "2026-07-27T02:00:02Z";
        auto* fault_payload = mqtt::GetPayload<mqtt::SensorStatusPayload>(sensor_fault);
        assert(fault_payload != nullptr);
        fault_payload->measurement_status = "FAULT";
        fault_payload->distance_cm = 0xFFFF;
        assert(handler.Handle(mqtt::DeviceEventTopic("PI-LINETRACER-01"), Encode(sensor_fault)));
        assert(qt_events.size() == 3);
        forwarded = mqtt::GetPayload<mqtt::SensorStatusPayload>(qt_events.back());
        assert(forwarded != nullptr);
        assert(forwarded->detection_status.has_value() && *forwarded->detection_status == "UNKNOWN");
    }
    std::filesystem::remove_all(root);
}

void TestHeartbeatTimeoutChangesAreForwardedToQt() {
    central_server::DeviceManager::Clock::time_point now{};
    central_server::DeviceManager device_manager({}, [&now] { return now; });
    central_server::MqttHandler handler(device_manager);
    std::vector<mqtt::MqttMessage> qt_statuses;
    handler.SetQtStatusHandler([&qt_statuses](const mqtt::MqttMessage& message) {
        qt_statuses.push_back(message);
        return true;
    });

    assert(handler.Handle("device/PI-01/register", Encode(MakeRegistration()), "2026-07-16T01:00:00Z"));
    assert(device_manager.HandleMessage(mqtt::ParseTopic("device/PI-01/status"), MakePositionStatus(),
                                        "2026-07-16T01:00:01Z"));
    std::vector<mqtt::ConnectionState> process_previews;
    std::vector<mqtt::ConnectionState> process_commits;
    handler.SetProcessMessageGuard([&process_previews](const mqtt::MqttMessage& message) {
        const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message);
        assert(status != nullptr);
        process_previews.push_back(status->status);
        return true;
    });
    handler.SetProcessMessageHandler([&process_commits](const mqtt::MqttMessage& message) {
        const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(message);
        assert(status != nullptr);
        process_commits.push_back(status->status);
        return true;
    });

    now += std::chrono::seconds(10);
    assert(handler.CheckHeartbeatTimeouts("2026-07-16T01:00:10Z"));
    assert(qt_statuses.size() == 1);
    const auto* delayed = mqtt::GetPayload<mqtt::DeviceStatusPayload>(qt_statuses[0]);
    assert(delayed != nullptr);
    assert(delayed->status == mqtt::ConnectionState::kDelayed);
    assert(process_previews == std::vector{ mqtt::ConnectionState::kDelayed });
    assert(process_commits == std::vector{ mqtt::ConnectionState::kDelayed });
    assert(!delayed->error_code.has_value());
    AssertPositionStatus(*delayed);
    assert(mqtt::ValidateTopicMessage(mqtt::QtStatusTopic("control-center"), qt_statuses[0]).IsSuccess());

    now += std::chrono::seconds(5);
    assert(handler.CheckHeartbeatTimeouts("2026-07-16T01:00:15Z"));
    assert(qt_statuses.size() == 2);
    const auto* offline = mqtt::GetPayload<mqtt::DeviceStatusPayload>(qt_statuses[1]);
    assert(offline != nullptr);
    assert(offline->status == mqtt::ConnectionState::kOffline);
    assert(offline->error_code == std::optional<std::string>("ERR-HEARTBEAT-TIMEOUT"));
    AssertPositionStatus(*offline);
    const std::vector expected_process_states{ mqtt::ConnectionState::kDelayed, mqtt::ConnectionState::kOffline };
    assert(process_previews == expected_process_states);
    assert(process_commits == expected_process_states);

    assert(handler.CheckHeartbeatTimeouts("2026-07-16T01:00:16Z"));
    assert(qt_statuses.size() == 2);
}

void TestRetainedPositionSnapshotReplay() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto registry_path =
        std::filesystem::temp_directory_path() / ("logistics-position-replay-" + unique + ".json");

    {
        central_server::DeviceManager device_manager(registry_path);
        assert(device_manager.HandleMessage(mqtt::ParseTopic("device/PI-01/register"), MakeRegistration(),
                                            "2026-07-16T01:00:01Z"));
        assert(device_manager.HandleMessage(mqtt::ParseTopic("device/PI-01/status"), MakePositionStatus(),
                                            "2026-07-16T01:00:03Z"));
    }

    {
        central_server::DeviceManager restored(registry_path);
        central_server::MqttHandler handler(restored);
        std::vector<mqtt::MqttMessage> qt_statuses;
        handler.SetQtStatusHandler([&qt_statuses](const mqtt::MqttMessage& message) {
            qt_statuses.push_back(message);
            return true;
        });

        assert(handler.ReplayDeviceStatuses("PI-01", "2026-07-16T01:05:00Z"));
        assert(qt_statuses.size() == 1);
        assert(qt_statuses[0].source_id == "PI-01");
        assert(qt_statuses[0].timestamp == "2026-07-16T01:00:03Z");
        const auto* status = mqtt::GetPayload<mqtt::DeviceStatusPayload>(qt_statuses[0]);
        assert(status != nullptr);
        assert(status->status == mqtt::ConnectionState::kOffline);
        AssertPositionStatus(*status);
        assert(!handler.ReplayDeviceStatuses("PI-NOT-REGISTERED", "2026-07-16T01:05:01Z"));
    }

    std::error_code error;
    std::filesystem::remove(registry_path, error);
}

void TestMessageTypesUseDedicatedRouteHandlers() {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() / ("logistics-routing-test-" + unique);
    std::filesystem::create_directories(root);
    {
        central_server::Database database;
        const central_server::DatabaseConfig database_config{
            .path = root / "test.db",
            .migration_dir = LOGISTICS_TEST_MIGRATION_DIR,
            .busy_timeout_ms = 100,
        };
        assert(database.Open(database_config).ok());
        assert(central_server::MigrationRunner::Apply(database, database_config.migration_dir).ok());

        central_server::StorageConfig storage;
        storage.image_root = root / "images";
        central_server::PersistenceService persistence(database, storage);
        central_server::DeviceManager device_manager;
        central_server::MqttHandler handler(device_manager, {}, &persistence);
        std::vector<mqtt::MessageType> command_routes;
        std::vector<mqtt::MessageType> response_routes;
        std::vector<mqtt::MessageType> status_routes;
        std::vector<mqtt::MessageType> error_routes;
        handler.SetCommandRouteHandler([&command_routes](const mqtt::MqttMessage& message) {
            command_routes.push_back(message.message_type);
            return true;
        });
        handler.SetQtResponseHandler([&response_routes](const mqtt::MqttMessage& message) {
            response_routes.push_back(message.message_type);
            return true;
        });
        handler.SetQtStatusHandler([&status_routes](const mqtt::MqttMessage& message) {
            status_routes.push_back(message.message_type);
            return true;
        });
        handler.SetQtErrorHandler([&error_routes](const mqtt::MqttMessage& message) {
            error_routes.push_back(message.message_type);
            return true;
        });

        assert(handler.Handle("device/PI-ROUTE/register", Encode(MakeRegistration("PI-ROUTE"))));

        const mqtt::MqttMessage command{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-ROUTE-COMMAND",
            .message_type = mqtt::MessageType::kControlCommand,
            .source_id = "QT-ROUTE",
            .timestamp = "2026-07-24T01:00:00Z",
            .data =
                mqtt::ControlCommandPayload{
                    .request_id = "REQ-ROUTE-1",
                    .command = mqtt::ControlCommand::kStart,
                    .target_device_id = "PI-ROUTE",
                    .component_id = "CONVEYOR-01",
                    .params = mqtt::Json::object(),
                },
        };
        assert(handler.Handle(mqtt::QtRequestTopic("QT-ROUTE"), Encode(command)));

        const mqtt::MqttMessage response{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-ROUTE-RESPONSE",
            .message_type = mqtt::MessageType::kCommandResponse,
            .source_id = "PI-ROUTE",
            .timestamp = "2026-07-24T01:00:01Z",
            .data =
                mqtt::CommandResponsePayload{
                    .request_id = "REQ-ROUTE-1",
                    .command = mqtt::ControlCommand::kStart,
                    .result = mqtt::CommandResult::kSuccess,
                    .error_code = std::nullopt,
                    .message = "started",
                },
        };
        assert(handler.Handle(mqtt::DeviceResponseTopic("PI-ROUTE"), Encode(response)));

        const mqtt::MqttMessage status{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-ROUTE-STATUS",
            .message_type = mqtt::MessageType::kDeviceStatus,
            .source_id = "PI-ROUTE",
            .timestamp = "2026-07-24T01:00:02Z",
            .data =
                mqtt::DeviceStatusPayload{
                    .status = mqtt::ConnectionState::kOnline,
                    .current_state = "RUNNING",
                    .job_id = std::nullopt,
                    .error_code = std::nullopt,
                },
        };
        assert(handler.Handle(mqtt::DeviceStatusTopic("PI-ROUTE"), Encode(status)));

        const mqtt::MqttMessage error{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-ROUTE-ERROR",
            .message_type = mqtt::MessageType::kErrorOccurred,
            .source_id = "PI-ROUTE",
            .timestamp = "2026-07-24T01:00:03Z",
            .data =
                mqtt::ErrorOccurredPayload{
                    .job_id = std::nullopt,
                    .error_code = "ERR-ROUTE-TEST",
                    .error_level = "WARNING",
                    .current_state = "RUNNING",
                    .message = "routing test",
                    .distance = std::nullopt,
                },
        };
        assert(handler.Handle(mqtt::DeviceErrorTopic("PI-ROUTE"), Encode(error)));

        assert(command_routes == std::vector{ mqtt::MessageType::kControlCommand });
        assert(response_routes == std::vector{ mqtt::MessageType::kCommandResponse });
        assert(status_routes == std::vector{ mqtt::MessageType::kDeviceStatus });
        assert(error_routes == std::vector{ mqtt::MessageType::kErrorOccurred });
    }
    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    TestRegistrationIsDecodedValidatedAndRouted();
    TestMalformedJsonIsRejected();
    TestTopicMessageMismatchIsRejected();
    TestUnsupportedVersionAndMissingFieldsAreRejected();
    TestUnknownMessageTypeLogIncludesReceivedTypeAndTopic();
    TestBarcodeUsesCatalogOrDefaultDestination();
    TestHeartbeatIsForwardedToQtAsDeviceStatus();
    TestSensorStatusIsAcceptedAndForwardedToQt();
    TestHeartbeatTimeoutChangesAreForwardedToQt();
    TestRetainedPositionSnapshotReplay();
    TestMessageTypesUseDedicatedRouteHandlers();
    return 0;
}
