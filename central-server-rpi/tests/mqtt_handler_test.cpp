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

}  // namespace

int main() {
    TestRegistrationIsDecodedValidatedAndRouted();
    TestMalformedJsonIsRejected();
    TestTopicMessageMismatchIsRejected();
    TestBarcodeIsEnrichedFromProductCatalog();
    return 0;
}
