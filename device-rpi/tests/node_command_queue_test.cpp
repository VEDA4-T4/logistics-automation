#include "logistics/device/node_command_queue.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

#include "logistics/contracts/mqtt_validation.hpp"

namespace {

namespace mqtt = logistics::contracts::mqtt;
using logistics::device::MakeTerminalCommandResponse;
using logistics::device::NodeCommandQueue;

mqtt::MqttMessage Control(const std::string& message_id, const mqtt::ControlCommand command) {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = message_id,
        .message_type = mqtt::MessageType::kControlCommand,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-29T10:00:00+09:00",
        .data =
            mqtt::ControlCommandPayload{
                .request_id = "REQ-" + message_id,
                .command = command,
                .target_device_id = "PI-INPUT-01",
                .component_id = {},
                .params = mqtt::Json::object(),
            },
    };
}

mqtt::MqttMessage EmergencyStop() {
    return {
        .protocol_version = std::string(mqtt::kCurrentProtocolVersion),
        .message_id = "MSG-ESTOP",
        .message_type = mqtt::MessageType::kEmergencyStop,
        .source_id = "SERVER-01",
        .timestamp = "2026-07-29T10:00:01+09:00",
        .data =
            mqtt::EmergencyStopPayload{
                .request_id = "REQ-ESTOP",
                .command = mqtt::ControlCommand::kEmergencyStop,
                .target_device_id = "ALL",
            },
    };
}

void TestEmergencyStopPreemptsNormalCommands() {
    NodeCommandQueue queue(3U);
    assert(queue.Push(Control("MSG-START", mqtt::ControlCommand::kStart)));
    assert(queue.Push(Control("MSG-STOP", mqtt::ControlCommand::kStop)));
    std::deque<mqtt::MqttMessage> preempted;
    assert(queue.Push(EmergencyStop(), &preempted));

    const auto emergency = queue.TryPopEmergency();
    assert(emergency.has_value());
    assert(emergency->message_id == "MSG-ESTOP");
    assert(preempted.size() == 2U);
    assert(preempted[0].message_id == "MSG-START");
    assert(preempted[1].message_id == "MSG-STOP");
    assert(!queue.TryPop().has_value());
    assert(queue.Size() == 0U);
}

void TestQueueCapacityIncludesBothPriorities() {
    NodeCommandQueue queue(2U);
    assert(queue.Push(Control("MSG-START", mqtt::ControlCommand::kStart)));
    assert(queue.Push(Control("MSG-STOP", mqtt::ControlCommand::kStop)));
    assert(!queue.Push(Control("MSG-START-2", mqtt::ControlCommand::kStart)));
    assert(queue.Push(EmergencyStop()));
    assert(queue.Push(Control("MSG-STOP-2", mqtt::ControlCommand::kStop)));
}

void TestEmergencyStopIsAcceptedWhenNormalQueueIsFull() {
    NodeCommandQueue queue(2U);
    assert(queue.Push(Control("MSG-START", mqtt::ControlCommand::kStart)));
    assert(queue.Push(Control("MSG-STOP", mqtt::ControlCommand::kStop)));
    std::deque<mqtt::MqttMessage> preempted;
    assert(queue.Push(EmergencyStop(), &preempted));
    assert(preempted.size() == 2U);
    assert(queue.Size() == 1U);
    assert(queue.TryPopEmergency().has_value());
}

void TestTerminalResponsePreservesRequest() {
    const auto response =
        MakeTerminalCommandResponse(Control("MSG-START", mqtt::ControlCommand::kStart), "PI-INPUT-01",
                                    "MSG-START-QUEUE-FULL", "2026-07-29T10:00:02+09:00", mqtt::CommandResult::kRejected,
                                    std::string("ERR-COMMAND-QUEUE-FULL"), "input command queue is full");
    assert(response.has_value());
    assert(mqtt::ValidateTopicMessage(mqtt::DeviceResponseTopic("PI-INPUT-01"), *response).IsSuccess());
    const auto* payload = mqtt::GetPayload<mqtt::CommandResponsePayload>(*response);
    assert(payload != nullptr);
    assert(payload->request_id == "REQ-MSG-START");
    assert(payload->command == mqtt::ControlCommand::kStart);
    assert(payload->result == mqtt::CommandResult::kRejected);
    assert(payload->error_code == "ERR-COMMAND-QUEUE-FULL");
}

void TestZeroCapacityIsRejected() {
    bool threw = false;
    try {
        static_cast<void>(NodeCommandQueue(0U));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}

}  // namespace

int main() {
    TestEmergencyStopPreemptsNormalCommands();
    TestQueueCapacityIncludesBothPriorities();
    TestEmergencyStopIsAcceptedWhenNormalQueueIsFull();
    TestTerminalResponsePreservesRequest();
    TestZeroCapacityIsRejected();
    return 0;
}
