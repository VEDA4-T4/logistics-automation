#include "logistics/device/input_uart_session.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "fake_input_uart_backend.hpp"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart_protocol.h"

namespace {

using input_test::AlwaysSucceed;
using input_test::AutoResponderBackend;
using input_test::MakeOperationResult;
using input_test::MakeSensorStatus;
using input_test::MakeStatusResponse;
using logistics::device::InputTransactResult;
using logistics::device::InputTransactStatus;
using logistics::device::InputUartSession;
using logistics::device::UartIoResult;

struct Fixture {
    Fixture() {
        auto owned = std::make_unique<AutoResponderBackend>();
        backend = owned.get();
        session = std::make_unique<InputUartSession>(std::move(owned));
        assert(session->Open());
    }

    AutoResponderBackend* backend{};
    std::unique_ptr<InputUartSession> session;
};

void TestTransactSuccess() {
    Fixture fixture;
    fixture.backend->responder = AlwaysSucceed();

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_START);

    assert(result.Succeeded());
    assert(result.response_command == UART_CMD_OPERATION_RESULT);
    assert(result.response_status == UART_STATUS_SUCCESS);
    assert(fixture.backend->last_written.command == UART_CMD_INPUT_CONVEYOR_START);
    assert(fixture.backend->write_calls == 1);
    assert(fixture.session->Diagnostics().responses_matched == 1);
}

void TestTransactStatusResponseCarriesData() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeStatusResponse(request.sequence, UART_INPUT_CONVEYOR_RUNNING, 80U) };
    };

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_GET_STATUS);

    assert(result.Succeeded());
    assert(result.response_command == UART_CMD_RESPONSE);
    assert(result.response_frame.payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX] == UART_INPUT_CONVEYOR_RUNNING);
    assert(result.response_frame.payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX] == 80U);
}

void TestTransactRejected() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeOperationResult(request.sequence, UART_STATUS_ERROR, UART_ERROR_MOTOR) };
    };

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_START);

    assert(!result.Succeeded());
    assert(result.status == InputTransactStatus::kRejected);
    assert(result.response_error == UART_ERROR_MOTOR);
    assert(fixture.backend->write_calls == 1);
}

void TestTransactTimeoutRetries() {
    Fixture fixture;
    fixture.backend->responder = [](const uart_frame_t&) { return std::vector<uart_frame_t>{}; };

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_STOP);

    assert(result.status == InputTransactStatus::kTimeout);
    assert(result.retries == UART_MAX_RETRY_COUNT);
    assert(fixture.backend->write_calls == static_cast<int>(UART_MAX_RETRY_COUNT) + 1);
    assert(fixture.session->Diagnostics().timeouts == 1);
    assert(fixture.session->Diagnostics().retries == UART_MAX_RETRY_COUNT);
}

void TestTransactRetryThenSuccess() {
    Fixture fixture;
    int calls = 0;
    fixture.backend->responder = [&calls](const uart_frame_t& request) {
        if (++calls == 1) {
            return std::vector<uart_frame_t>{};
        }
        return std::vector<uart_frame_t>{ MakeOperationResult(request.sequence, UART_STATUS_SUCCESS, UART_ERROR_NONE) };
    };

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_START);

    assert(result.Succeeded());
    assert(result.retries == 1);
    assert(fixture.backend->write_calls == 2);
}

void TestSpontaneousFrameDuringWait() {
    Fixture fixture;
    int spontaneous = 0;
    std::uint8_t spontaneous_command = 0U;
    fixture.session->SetSpontaneousFrameHandler([&](const uart_frame_t& frame) {
        ++spontaneous;
        spontaneous_command = frame.command;
    });
    fixture.backend->responder = [](const uart_frame_t& request) {
        return std::vector<uart_frame_t>{ MakeSensorStatus(UART_SENSOR_DETECTED, 12U),
                                          MakeOperationResult(request.sequence, UART_STATUS_SUCCESS, UART_ERROR_NONE) };
    };

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_START);

    assert(result.Succeeded());
    assert(spontaneous == 1);
    assert(spontaneous_command == UART_CMD_SENSOR_STATUS);
    assert(fixture.session->Diagnostics().spontaneous_frames == 1);
}

/*
 * Regression: this reproduces a live-hardware bug found debugging the gripper
 * controller. When our response and the controller's next unrelated frame (its
 * periodic health-task heartbeat, in the field case) arrive in the same
 * Read() -- which is exactly why kReceiveBufferSize is sized for two frames --
 * WaitForResponse used to return the instant it matched our response, leaving
 * the trailing bytes already pulled out of receive_buffer un-fed to the
 * parser. They were not deferred to the next Read(): they were gone, and the
 * frame they belonged to could never complete.
 */
void TestTrailingFrameInSameReadIsNotDropped() {
    Fixture fixture;
    int spontaneous = 0;
    std::uint8_t spontaneous_command = 0U;
    fixture.session->SetSpontaneousFrameHandler([&](const uart_frame_t& frame) {
        ++spontaneous;
        spontaneous_command = frame.command;
    });
    // No responder is set, so Write() queues nothing on its own; the reply
    // comes entirely from the combined chunk queued below, ahead of time.
    fixture.backend->PreloadCombinedIncoming(
        { MakeOperationResult(1U, UART_STATUS_SUCCESS, UART_ERROR_NONE), MakeSensorStatus(UART_SENSOR_DETECTED, 7U) });

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_START);

    assert(result.Succeeded());
    // Before the fix, this stayed 0: the sensor frame's bytes were already
    // consumed from the fake Read() but never reached the parser.
    assert(spontaneous == 1);
    assert(spontaneous_command == UART_CMD_SENSOR_STATUS);
    assert(fixture.session->Diagnostics().spontaneous_frames == 1);
}

void TestPollSpontaneous() {
    Fixture fixture;
    int spontaneous = 0;
    fixture.session->SetSpontaneousFrameHandler([&](const uart_frame_t& frame) {
        assert(frame.command == UART_CMD_SENSOR_STATUS);
        ++spontaneous;
    });
    fixture.backend->PreloadIncoming(MakeSensorStatus(UART_SENSOR_CLEAR, 40U));

    const UartIoResult io = fixture.session->PollSpontaneous(std::chrono::milliseconds{ 5 });

    assert(io.Succeeded());
    assert(spontaneous == 1);
}

void TestTransactNotOpen() {
    InputUartSession session(std::make_unique<AutoResponderBackend>());

    const InputTransactResult result = session.Transact(UART_CMD_INPUT_CONVEYOR_START);

    assert(result.status == InputTransactStatus::kNotOpen);
}

void TestTransactInvalidArgument() {
    Fixture fixture;
    const std::array<std::uint8_t, 1> payload{ 0x01 };

    const InputTransactResult result = fixture.session->Transact(0x99U, payload);

    assert(result.status == InputTransactStatus::kInvalidArgument);
    assert(fixture.backend->write_calls == 0);
}

void TestWriteTransportError() {
    Fixture fixture;
    fixture.backend->fail_write = true;

    const InputTransactResult result = fixture.session->Transact(UART_CMD_INPUT_CONVEYOR_START);

    assert(result.status == InputTransactStatus::kTransportError);
    assert(!fixture.session->IsOpen());
}

void TestSendCommandDoesNotWaitForReply() {
    Fixture fixture;
    // No responder is set, so no reply is ever queued; SendCommand must not
    // block waiting for one.
    const InputTransactResult result = fixture.session->SendCommand(UART_CMD_EMERGENCY_STOP);

    assert(result.status == InputTransactStatus::kSent);
    assert(fixture.backend->write_calls == 1);
    assert(fixture.backend->last_written.command == UART_CMD_EMERGENCY_STOP);
}

void TestSendCommandWriteTransportError() {
    Fixture fixture;
    fixture.backend->fail_write = true;

    const InputTransactResult result = fixture.session->SendCommand(UART_CMD_EMERGENCY_STOP);

    assert(result.status == InputTransactStatus::kTransportError);
}

}  // namespace

int main() {
    TestTransactSuccess();
    TestTransactStatusResponseCarriesData();
    TestTransactRejected();
    TestTransactTimeoutRetries();
    TestTransactRetryThenSuccess();
    TestSpontaneousFrameDuringWait();
    TestTrailingFrameInSameReadIsNotDropped();
    TestPollSpontaneous();
    TestTransactNotOpen();
    TestTransactInvalidArgument();
    TestWriteTransportError();
    TestSendCommandDoesNotWaitForReply();
    TestSendCommandWriteTransportError();
    return 0;
}
