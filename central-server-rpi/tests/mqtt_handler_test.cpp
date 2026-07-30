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

void TestBarcodeIsEnrichedFromProductCatalog() {
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
        central_server::MqttHandler handler(device_manager, {}, &persistence);
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
        central_server::MqttHandler handler(device_manager, {}, &persistence);
        std::vector<mqtt::MqttMessage> qt_events;
        handler.SetQtEventHandler([&qt_events](const mqtt::MqttMessage& message) {
            qt_events.push_back(message);
            return true;
        });

        const mqtt::MqttMessage sensor_status{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-SENSOR-STATUS-01",
            .message_type = mqtt::MessageType::kSensorStatus,
            .source_id = "PI-INPUT-01",
            .timestamp = "2026-07-27T02:00:00Z",
            .data =
                mqtt::SensorStatusPayload{
                    .sensor_id = 1,
                    .measurement_status = "DETECTED",
                    .distance_cm = 14,
                },
        };
        assert(handler.Handle(mqtt::DeviceEventTopic("PI-INPUT-01"), Encode(sensor_status)));
        assert(qt_events.size() == 1);
        const auto* forwarded = mqtt::GetPayload<mqtt::SensorStatusPayload>(qt_events.front());
        assert(forwarded != nullptr);
        assert(forwarded->sensor_id == 1);
        assert(forwarded->measurement_status == "DETECTED");
        assert(forwarded->distance_cm == 14);
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
    assert(mqtt::ValidateTopicMessage(mqtt::QtStatusTopic("control-center"), qt_statuses[0]).IsSuccess());

    now += std::chrono::seconds(5);
    assert(handler.CheckHeartbeatTimeouts("2026-07-16T01:00:15Z"));
    assert(qt_statuses.size() == 2);
    const auto* offline = mqtt::GetPayload<mqtt::DeviceStatusPayload>(qt_statuses[1]);
    assert(offline != nullptr);
    assert(offline->status == mqtt::ConnectionState::kOffline);
    assert(offline->error_code == std::optional<std::string>("ERR-HEARTBEAT-TIMEOUT"));
    const std::vector expected_process_states{ mqtt::ConnectionState::kDelayed, mqtt::ConnectionState::kOffline };
    assert(process_previews == expected_process_states);
    assert(process_commits == expected_process_states);

    assert(handler.CheckHeartbeatTimeouts("2026-07-16T01:00:16Z"));
    assert(qt_statuses.size() == 2);
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
    TestBarcodeIsEnrichedFromProductCatalog();
    TestHeartbeatIsForwardedToQtAsDeviceStatus();
    TestSensorStatusIsAcceptedAndForwardedToQt();
    TestHeartbeatTimeoutChangesAreForwardedToQt();
    TestMessageTypesUseDedicatedRouteHandlers();
    return 0;
}
