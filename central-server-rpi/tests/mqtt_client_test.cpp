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

    void Stop() noexcept override {
        ++stop_count;
    }

    [[nodiscard]] central_server::MqttOperationResult Publish(std::string_view topic, std::string_view payload, int qos,
                                                              bool retain) override {
        publications.push_back({ std::string(topic), std::string(payload), qos, retain });
        return publish_result;
    }

    [[nodiscard]] central_server::MqttOperationResult Subscribe(std::string_view topic_filter, int qos) override {
        subscriptions.push_back({ std::string(topic_filter), qos });
        return subscribe_result;
    }

    void SimulateConnected(int reason_code = 0, std::string_view reason = "connection accepted") const {
        callbacks_.connected(reason_code, reason);
    }

    void SimulateDisconnected(int reason_code, std::string_view reason) const {
        callbacks_.disconnected(reason_code, reason);
    }

    void SimulateMessage(std::string_view topic, std::string_view payload) const {
        callbacks_.message_received(topic, payload);
    }

    central_server::MqttTransportCallbacks callbacks_;
    central_server::MqttOperationResult start_result{};
    central_server::MqttOperationResult publish_result{};
    central_server::MqttOperationResult subscribe_result{};
    central_server::MqttTransportOptions last_options;
    std::vector<Subscription> subscriptions;
    std::vector<Publication> publications;
    int start_count{};
    int stop_count{};
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
    assert(fake->subscriptions.size() == 7);
    assert(fake->subscriptions.front().topic == mqtt::kServerRequestSubscription);
    assert(fake->subscriptions.back().topic == mqtt::kDeviceHeartbeatSubscription);

    assert(client.Publish(mqtt::kServerStatusTopic, R"json({"status":"ONLINE"})json", 1, true));
    assert(fake->publications.size() == 1);
    assert(fake->publications.front().topic == mqtt::kServerStatusTopic);
    assert(fake->publications.front().qos == 1);
    assert(fake->publications.front().retain);
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

void TestMessagesAreForwarded() {
    auto transport = std::make_unique<FakeTransport>();
    auto* fake = transport.get();
    central_server::MqttClient client(MakeConfig(), std::move(transport));

    std::string received_topic;
    std::string received_payload;
    client.SetMessageHandler([&](std::string_view topic, std::string_view payload) {
        received_topic = topic;
        received_payload = payload;
    });

    assert(client.Start());
    fake->SimulateMessage("device/PI-01/status", "payload");
    assert(received_topic == "device/PI-01/status");
    assert(received_payload == "payload");
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

    assert(!client.PublishMessage(mqtt::DeviceCommandTopic("PI-02"), command, mqtt::Qos::kAtLeastOnce));
    assert(!client.PublishMessage(mqtt::DeviceCommandTopic("PI-01"), command, mqtt::Qos::kAtLeastOnce, true));
    assert(fake->publications.size() == 1);

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
    TestMessagesAreForwarded();
    TestTypedMessagePublishing();
    TestStartFailureIsLogged();
    TestConnectionRejectionIsLogged();
    return 0;
}
