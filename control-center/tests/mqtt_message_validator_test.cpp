#include "logistics/control_center/mqtt_message_validator.hpp"

#include <QByteArray>
#include <QString>
#include <cassert>

int main() {
    using logistics::control_center::MqttMessageValidator;

    const QByteArray valid_payload = R"({
        "protocolVersion":"1.0",
        "messageId":"MSG-0001",
        "messageType":"DEVICE_STATUS",
        "sourceId":"server-01",
        "timestamp":"2026-07-14T01:02:03.000Z",
        "data":{"state":"ONLINE"}
    })";

    assert(MqttMessageValidator::Validate("qt/control-center/status", valid_payload, "control-center").is_valid);
    assert(MqttMessageValidator::Validate("server/status", valid_payload, "control-center").is_valid);
    assert(!MqttMessageValidator::Validate("qt/another-client/status", valid_payload, "control-center").is_valid);
    assert(!MqttMessageValidator::Validate("device/PI-01/status", valid_payload, "control-center").is_valid);
    assert(!MqttMessageValidator::Validate("qt/control-center/status", "{", "control-center").is_valid);

    auto invalid_protocol = valid_payload;
    invalid_protocol.replace("\"1.0\"", "\"2.0\"");
    assert(!MqttMessageValidator::Validate("qt/control-center/status", invalid_protocol, "control-center").is_valid);

    auto invalid_timestamp = valid_payload;
    invalid_timestamp.replace("2026-07-14T01:02:03.000Z", "2026-07-14T01:02:03");
    assert(!MqttMessageValidator::Validate("qt/control-center/status", invalid_timestamp, "control-center").is_valid);

    return 0;
}
