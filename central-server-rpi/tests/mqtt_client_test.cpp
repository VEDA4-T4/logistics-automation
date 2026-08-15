#include "logistics/central_server/mqtt_client.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "logistics/central_server/mqtt_config.hpp"
#include "logistics/central_server/mqtt_transport.hpp"
#include "logistics/contracts/mqtt_codec.hpp"
#include "logistics/contracts/mqtt_message.hpp"
#include "logistics/contracts/mqtt_topic.hpp"

namespace {

namespace central_server = logistics::central_server;
namespace mqtt = logistics::contracts::mqtt;

struct Subscription final {
    std::string topic;
    int qos{};
};

struct Publication final {
    std::string topic;
    std::string payload;
    int qos{};
    bool retain{};
};

class FakeTransport final : public central_server::MqttTransport {
public:
    void SetCallbacks(central_server::MqttTransportCallbacks callbacks) override {
        callbacks_ = std::move(callbacks);
    }

    [[nodiscard]] central_server::MqttOperationResult Start(
        const central_server::MqttTransportOptions& options) override {
        ++start_count;
        last_options = options;
        return start_result;
    }

    void Stop(bool graceful = true) noexcept override {
        ++stop_count;
        last_stop_graceful = graceful;
    }

    [[nodiscard]] central_server::MqttOperationResult RequestReconnect() override {
        ++reconnect_request_count;
        return reconnect_result;
    }

    [[nodiscard]] central_server::MqttOperationResult Publish(std::string_view topic, std::string_view payload, int qos,
                                                              bool retain) override {
        publications.push_back({ std::string(topic), std::string(payload), qos, retain });
        auto result = publish_result;
        result.packet_id = result.packet_id == 0 ? next_packet_id_++ : result.packet_id;
        if (disconnect_during_publish) {
            callbacks_.disconnected(7, "connection lost during publish");
        } else if (acknowledge_during_publish ||
                   (acknowledge_offline_during_publish && payload.find("OFFLINE") != std::string_view::npos)) {
            callbacks_.publish_acknowledged(result.packet_id);
        }
        return result;
    }

    [[nodiscard]] central_server::MqttOperationResult Subscribe(std::string_view topic_filter, int qos) override {
        subscriptions.push_back({ std::string(topic_filter), qos });
        auto result = subscribe_result;
        result.packet_id = result.packet_id == 0 ? next_packet_id_++ : result.packet_id;
        if (acknowledge_during_subscribe) {
            callbacks_.subscribe_acknowledged(result.packet_id, { qos });
        }
        return result;
    }

    void SimulateConnected(int reason_code = 0, std::string_view reason = "connection accepted") const {
        callbacks_.connected(reason_code, reason);
    }

    void SimulateDisconnected(int reason_code, std::string_view reason) const {
        callbacks_.disconnected(reason_code, reason);
    }

    void SimulateMessage(std::string_view topic, std::string_view payload, int qos = 1, bool retained = false) const {
        callbacks_.message_received(topic, payload, qos, retained);
    }

    void SimulatePublishAcknowledged(int packet_id) const {
        callbacks_.publish_acknowledged(packet_id);
    }

    void SimulateSubscribeAcknowledged(int packet_id, std::vector<int> granted_qos) const {
        callbacks_.subscribe_acknowledged(packet_id, granted_qos);
    }

    [[nodiscard]] int LastPacketId() const noexcept {
        return next_packet_id_ - 1;
    }

    central_server::MqttTransportCallbacks callbacks_;
    central_server::MqttOperationResult start_result{};
    central_server::MqttOperationResult publish_result{};
    central_server::MqttOperationResult subscribe_result{};
    central_server::MqttOperationResult reconnect_result{};
    central_server::MqttTransportOptions last_options;
    std::vector<Subscription> subscriptions;
    std::vector<Publication> publications;
    int start_count{};
    int stop_count{};
    int reconnect_request_count{};
    bool acknowledge_during_publish{};
    bool acknowledge_offline_during_publish{ true };
    bool disconnect_during_publish{};
    bool acknowledge_during_subscribe{};
    bool last_stop_graceful{ true };

private:
    int next_packet_id_{ 1 };
};

[[nodiscard]] std::filesystem::path MakeTemporaryConfigPath(std::string_view suffix) {
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("logistics-server-" + std::to_string(timestamp) + "-" + std::string(suffix) + ".ini");
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path);
    assert(output);
    output << text;
    assert(output.good());
}

void TestConfigLoading() {
    const auto path = MakeTemporaryConfigPath("valid");
    WriteText(path, R"ini(
[mqtt]
host=192.168.10.20
port=1884
client_id=central-server-test
username=server
password=secret
tls_enabled=false
ca_certificate=
keep_alive_seconds=45
reconnect_min_delay_seconds=2
reconnect_max_delay_seconds=20
clean_session=yes

[device_registry]
path=registry/devices.json

[database]
path=/var/lib/logistics/logistics.db
)ini");

    const auto config = central_server::LoadMqttConfig(path);
    assert(config.host == "192.168.10.20");
    assert(config.port == 1884);
    assert(config.client_id == "central-server-test");
    assert(config.username == "server");
    assert(config.password == "secret");
    assert(config.keep_alive_seconds == 45);
    assert(config.reconnect_min_delay_seconds == 2);
    assert(config.reconnect_max_delay_seconds == 20);
    assert(config.clean_session);
    assert(config.device_registry_path == path.parent_path() / "registry/devices.json");
    assert(config.IsValid());

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestTlsWithoutCaIsRejected() {
    const auto path = MakeTemporaryConfigPath("tls-without-ca");

    {
        std::ofstream output(path);
        assert(output);
        output << R"ini(
[mqtt]
host=127.0.0.1
port=8883
client_id=central-server
tls_enabled=true
ca_certificate=
)ini";
    }

    bool rejected = false;
    try {
        static_cast<void>(logistics::central_server::LoadMqttConfig(path));
    } catch (const logistics::central_server::ConfigError&) {
        rejected = true;
    }

    assert(rejected);

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestInvalidConfigIsRejected() {
    const auto path = MakeTemporaryConfigPath("invalid");
    WriteText(path, R"ini(
[mqtt]
host=127.0.0.1
port=70000
client_id=central-server
)ini");

    bool rejected = false;
    try {
        static_cast<void>(central_server::LoadMqttConfig(path));
    } catch (const central_server::ConfigError&) {
        rejected = true;
    }
    assert(rejected);

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestPersistentSessionIsDefault() {
    const auto path = MakeTemporaryConfigPath("persistent-default");
    WriteText(path, R"ini(
[mqtt]
host=127.0.0.1
client_id=central-server
)ini");

    const auto config = central_server::LoadMqttConfig(path);
    assert(!config.clean_session);

    std::error_code error;
    std::filesystem::remove(path, error);
}

[[nodiscard]] central_server::MqttConfig MakeConfig() {
    return {
        .host = "127.0.0.1",
        .client_id = "central-server-test",
        .username = {},
        .password = {},
        .device_registry_path = {},
        .port = 1883,
        .keep_alive_seconds = 30,
        .reconnect_min_delay_seconds = 1,
        .reconnect_max_delay_seconds = 16,
        .clean_session = true,
        .tls_enabled = false,
        .ca_certificate = {},
    };
}

void TestConnectReconnectAndPublish() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    std::vector<std::string> logs;
    client.SetLogger([&logs](central_server::MqttLogLevel, std::string_view message) { logs.emplace_back(message); });

    assert(client.SetWill({
        .topic = std::string(mqtt::kServerStatusTopic),
        .payload = R"json({"status":"OFFLINE"})json",
        .qos = 1,
        .retain = true,
    }));

    assert(client.Start());
    assert(fake->start_count == 1);
    assert(fake->last_options.host == "127.0.0.1");
    assert(fake->last_options.reconnect_max_delay_seconds == 16);
    assert(fake->last_options.will.has_value());
    assert(fake->last_options.will->topic == mqtt::kServerStatusTopic);
    assert(fake->last_options.will->retain);
    assert(!client.SetWill({
        .topic = std::string(mqtt::kServerStatusTopic),
        .payload = "{}",
        .qos = 1,
        .retain = true,
    }));
    assert(!client.IsConnected());

    fake->SimulateConnected();
    assert(client.IsConnected());
    assert(!client.IsReady());
    assert(fake->subscriptions.size() == 7);
    assert(fake->subscriptions.front().topic == mqtt::kServerRequestSubscription);
    assert(fake->subscriptions.back().topic == mqtt::kDeviceHeartbeatSubscription);

    for (int packet_id = 1; packet_id <= 7; ++packet_id) {
        fake->SimulateSubscribeAcknowledged(packet_id, { 1 });
    }
    assert(client.IsReady());
    assert(fake->publications.size() == 2);
    assert(fake->publications[0].topic == mqtt::kServerStatusTopic);
    assert(fake->publications[0].qos == 1);
    assert(fake->publications[0].retain);
    assert(fake->publications[1].topic == mqtt::kServerHeartbeatTopic);
    assert(fake->publications[1].qos == 0);
    assert(!fake->publications[1].retain);

    assert(client.Publish(mqtt::kServerStatusTopic, R"json({"status":"ONLINE"})json", 1, true));
    assert(fake->publications.size() == 3);
    assert(fake->publications.back().topic == mqtt::kServerStatusTopic);
    assert(fake->publications.back().qos == 1);
    assert(fake->publications.back().retain);
    assert(!client.Publish("server/+/status", "{}"));

    fake->SimulateDisconnected(7, "connection lost");
    assert(!client.IsConnected());
    assert(!client.Publish(mqtt::kServerStatusTopic, "{}"));

    fake->SimulateConnected();
    assert(client.IsConnected());
    assert(fake->subscriptions.size() == 14);

    bool reconnect_logged = false;
    for (const auto& log : logs) {
        reconnect_logged = reconnect_logged || log.find("automatic reconnect") != std::string::npos;
    }
    assert(reconnect_logged);

    client.Stop();
    assert(fake->stop_count == 1);
}

void TestSubscribeRejectionKeepsClientUnreadyUntilReconnect() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    assert(client.Start());
    fake->SimulateConnected();
    fake->SimulateSubscribeAcknowledged(1, { 0x80 });
    assert(fake->reconnect_request_count == 1);
    for (int packet_id = 2; packet_id <= 7; ++packet_id) {
        fake->SimulateSubscribeAcknowledged(packet_id, { 1 });
    }
    assert(!client.IsReady());
    assert(fake->publications.empty());

    fake->SimulateDisconnected(7, "connection lost");
    fake->SimulateConnected();
    for (int packet_id = 8; packet_id <= 14; ++packet_id) {
        fake->SimulateSubscribeAcknowledged(packet_id, { 1 });
    }
    assert(client.IsReady());
    client.Stop();
}

void TestDowngradedSubscriptionQosRequestsReconnect() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    assert(client.Start());
    fake->SimulateConnected();
    fake->SimulateSubscribeAcknowledged(1, { 0 });
    assert(!client.IsReady());
    assert(fake->reconnect_request_count == 1);

    fake->SimulateDisconnected(0, "required QoS was downgraded");
    fake->SimulateConnected();
    for (int packet_id = 8; packet_id <= 14; ++packet_id) {
        fake->SimulateSubscribeAcknowledged(packet_id, { 1 });
    }
    assert(client.IsReady());
    client.Stop();
}

void TestStopWaitsForOfflineAcknowledgement() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    assert(client.Start());
    fake->SimulateConnected();
    client.Stop();

    assert(fake->last_stop_graceful);
    assert(!fake->publications.empty());
    assert(fake->publications.back().topic == mqtt::kServerStatusTopic);
    assert(fake->publications.back().qos == 1);
    assert(fake->publications.back().retain);
    assert(fake->publications.back().payload.find("OFFLINE") != std::string::npos);
}

void TestStopFallsBackToLastWillWhenOfflineIsNotAcknowledged() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    fake->acknowledge_offline_during_publish = false;
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    assert(client.Start());
    fake->SimulateConnected();
    client.Stop();

    assert(!fake->last_stop_graceful);
}

void TestLocalSubscribeFailureRequestsReconnect() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    fake->subscribe_result = { .code = 4, .message = "subscription request failed" };
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    assert(client.Start());
    fake->SimulateConnected();
    assert(!client.IsReady());
    assert(fake->reconnect_request_count == 1);

    fake->subscribe_result = {};
    fake->SimulateDisconnected(7, "connection lost");
    fake->SimulateConnected();
    for (int packet_id = 8; packet_id <= 14; ++packet_id) {
        fake->SimulateSubscribeAcknowledged(packet_id, { 1 });
    }
    assert(client.IsReady());
    assert(fake->publications.size() == 2);
    assert(fake->publications[0].topic == mqtt::kServerStatusTopic);
    client.Stop();
}

void TestSynchronousSubscribeAcknowledgementsAreNotLost() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    fake->acknowledge_during_subscribe = true;
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    assert(client.Start());
    fake->SimulateConnected();

    assert(client.IsReady());
    assert(fake->subscriptions.size() == 7);
    assert(fake->publications.size() == 2);
    client.Stop();
}

void TestMessagesAreForwarded() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    std::string received_topic;
    std::string received_payload;
    int received_qos = -1;
    bool received_retained = false;
    client.SetMessageHandler([&](std::string_view topic, std::string_view payload, int qos, bool retained) {
        received_topic = topic;
        received_payload = payload;
        received_qos = qos;
        received_retained = retained;
    });

    assert(client.Start());
    fake->SimulateMessage("device/PI-01/status", "payload", 0, true);
    assert(received_topic == "device/PI-01/status");
    assert(received_payload == "payload");
    assert(received_qos == 0);
    assert(received_retained);
}

void TestPublishReceiptAndAcknowledgementAreForwarded() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    int acknowledged_packet_id = 0;
    assert(client.Start());
    fake->SimulateConnected();
    assert(client.PublishMessageAcknowledged(
        mqtt::kServerStatusTopic,
        mqtt::MqttMessage{
            .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
            .message_id = "MSG-PUBLISH-ACK-01",
            .message_type = mqtt::MessageType::kDeviceStatus,
            .source_id = "SERVER-01",
            .timestamp = "2026-07-15T17:30:00+09:00",
            .data = mqtt::DeviceStatusPayload{ .status = mqtt::ConnectionState::kOnline, .current_state = "ONLINE" },
        },
        mqtt::Qos::kAtLeastOnce, [&acknowledged_packet_id](int packet_id) { acknowledged_packet_id = packet_id; },
        true));
    const int receipt = fake->LastPacketId();
    fake->SimulatePublishAcknowledged(receipt);
    assert(acknowledged_packet_id == receipt);
    client.Stop();
}

void TestSynchronousPublishAcknowledgementIsNotLost() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    fake->acknowledge_during_publish = true;
    central_server::MqttClient client(MakeConfig(), std::move(transport));
    int acknowledged_packet_id = 0;

    assert(client.Start());
    fake->SimulateConnected();
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-SYNC-PUBLISH-ACK-01",
        .message_type = mqtt::MessageType::kDeviceStatus,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-15T17:30:00+09:00",
        .data = mqtt::DeviceStatusPayload{ .status = mqtt::ConnectionState::kOnline, .current_state = "ONLINE" },
    };
    assert(client.PublishMessageAcknowledged(
        mqtt::kServerStatusTopic, message, mqtt::Qos::kAtLeastOnce,
        [&acknowledged_packet_id](int packet_id) { acknowledged_packet_id = packet_id; }, true));
    assert(acknowledged_packet_id == fake->LastPacketId());
    client.Stop();
}

void TestDisconnectDuringAcknowledgedPublishDoesNotRetainCallback() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    fake->disconnect_during_publish = true;
    central_server::MqttClient client(MakeConfig(), std::move(transport));
    int acknowledgements = 0;

    assert(client.Start());
    fake->SimulateConnected();
    const mqtt::MqttMessage message{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-DISCONNECT-PUBLISH-ACK-01",
        .message_type = mqtt::MessageType::kDeviceStatus,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-15T17:30:00+09:00",
        .data = mqtt::DeviceStatusPayload{ .status = mqtt::ConnectionState::kOnline, .current_state = "ONLINE" },
    };
    assert(!client.PublishMessageAcknowledged(
        mqtt::kServerStatusTopic, message, mqtt::Qos::kAtLeastOnce, [&acknowledgements](int) { ++acknowledgements; },
        true));
    fake->SimulatePublishAcknowledged(fake->LastPacketId());
    assert(acknowledgements == 0);
    client.Stop();
}

void TestTypedMessagePublishing() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    assert(client.Start());
    fake->SimulateConnected();

    const mqtt::MqttMessage command{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-PUBLISH-01",
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-15T17:30:00+09:00",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = "REQ-PUBLISH-01",
                .command = mqtt::ControlCommand::kStatusRequest,
                .target_device_id = "PI-01",
                .component_id = {},
                .params = mqtt::Json::object(),
            },
    };

    assert(client.PublishMessage(mqtt::DeviceCommandTopic("PI-01"), command, mqtt::Qos::kAtLeastOnce));
    assert(fake->publications.size() == 1);
    assert(fake->publications.front().topic == "device/PI-01/command");
    assert(fake->publications.front().qos == 1);
    assert(!fake->publications.front().retain);

    const auto decoded = mqtt::DeserializeMessage(fake->publications.front().payload);
    assert(decoded.IsSuccess());
    assert(decoded.value.message_id == command.message_id);
    assert(mqtt::GetPayload<mqtt::ControlCommandPayload>(decoded.value) != nullptr);
    assert(!client.PublishTransientMessage(mqtt::DeviceCommandTopic("PI-01"), command));
    assert(fake->publications.size() == 1);

    mqtt::MqttMessage sensor{
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-SENSOR-0",
        .message_type = mqtt::MessageType::kSensorStatus,
        .source_id = "central-server",
        .timestamp = "2026-08-15T03:00:00Z",
        .data =
            mqtt::SensorStatusPayload{
                .sensor_id = 1, .measurement_status = "OK", .distance_cm = 20, .detection_status = "CLEAR" },
    };
    for (int index = 0; index < 10'000; ++index) {
        sensor.message_id = "MSG-SENSOR-" + std::to_string(index);
        assert(client.PublishTransientMessage(mqtt::QtEventTopic("QT-01"), sensor));
    }
    assert(fake->publications.size() == 10'001);
    assert(fake->publications.front().topic == "device/PI-01/command");
    for (auto publication = fake->publications.begin() + 1; publication != fake->publications.end(); ++publication) {
        assert(publication->topic == "qt/QT-01/event");
        assert(publication->qos == 0);
        assert(!publication->retain);
    }
    assert(!client.PublishMessage(mqtt::QtEventTopic("QT-01"), sensor, mqtt::Qos::kAtLeastOnce));

    assert(!client.PublishMessage(mqtt::DeviceCommandTopic("PI-02"), command, mqtt::Qos::kAtLeastOnce));
    assert(!client.PublishMessage(mqtt::DeviceCommandTopic("PI-01"), command, mqtt::Qos::kAtLeastOnce, true));
    assert(fake->publications.size() == 10'001);

    client.Stop();
}

void TestStartFailureIsLogged() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    fake->start_result = { .code = 3, .message = "transport startup failed" };
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    std::vector<std::string> logs;
    client.SetLogger([&logs](central_server::MqttLogLevel, std::string_view message) { logs.emplace_back(message); });

    assert(!client.Start());
    assert(!logs.empty());
    assert(logs.back().find("transport startup failed") != std::string::npos);
}

void TestConnectionRejectionIsLogged() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    std::vector<std::string> logs;
    client.SetLogger([&logs](central_server::MqttLogLevel, std::string_view message) { logs.emplace_back(message); });

    assert(client.Start());
    fake->SimulateConnected(5, "not authorized");
    assert(!client.IsConnected());
    assert(fake->subscriptions.empty());

    bool rejection_logged = false;
    for (const auto& log : logs) {
        rejection_logged = rejection_logged || log.find("rejected the connection") != std::string::npos;
    }
    assert(rejection_logged);
    client.Stop();
}

}  // namespace

int main() {
    TestConfigLoading();
    TestInvalidConfigIsRejected();
    TestPersistentSessionIsDefault();
    TestConnectReconnectAndPublish();
    TestSubscribeRejectionKeepsClientUnreadyUntilReconnect();
    TestDowngradedSubscriptionQosRequestsReconnect();
    TestLocalSubscribeFailureRequestsReconnect();
    TestSynchronousSubscribeAcknowledgementsAreNotLost();
    TestStopWaitsForOfflineAcknowledgement();
    TestStopFallsBackToLastWillWhenOfflineIsNotAcknowledged();
    TestMessagesAreForwarded();
    TestPublishReceiptAndAcknowledgementAreForwarded();
    TestSynchronousPublishAcknowledgementIsNotLost();
    TestDisconnectDuringAcknowledgedPublishDoesNotRetainCallback();
    TestTypedMessagePublishing();
    TestStartFailureIsLogged();
    TestConnectionRejectionIsLogged();
    TestTlsWithoutCaIsRejected();
    return 0;
}
