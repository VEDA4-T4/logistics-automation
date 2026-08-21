#include <cassert>
#include <cstdint>

extern "C" {
#include "unload_config.h"
#include "unload_logic.h"
}

namespace {

app_unload_command_t MakeStart(std::uint16_t job_id, uart_linetracer_route_t route_id, std::uint32_t now_ms) {
    app_unload_command_t command{};

    command.type = APP_UNLOAD_COMMAND_START;
    command.requested_at_ms = now_ms;
    command.inhibit_generation = 7U;
    command.request_id = 11U;
    command.job_id = job_id;
    command.route_id = route_id;
    return command;
}

void TestCompletionAfterTimedServoCycle() {
    unload_logic_context_t context{};
    app_unload_result_t result{};
    const auto command = MakeStart(101U, UART_LINETRACER_ROUTE_B, 100U);

    UnloadLogic_Init(&context, 0U);
    assert(UnloadLogic_Start(&context, &command, 100U) != 0U);
    assert(UnloadLogic_GetServoOutput(&context) == UNLOAD_SERVO_OUTPUT_RELEASE);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS - 1U);
    assert(context.state == UNLOAD_LOGIC_MOVING_TO_RELEASE);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS);
    assert(context.state == UNLOAD_LOGIC_MOVING_HOME);
    assert(UnloadLogic_GetServoOutput(&context) == UNLOAD_SERVO_OUTPUT_HOME);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT,
                       100U + UNLOAD_SERVO_DEPLOY_MS + UNLOAD_SERVO_HOME_MS - 1U);
    assert(context.state == UNLOAD_LOGIC_MOVING_HOME);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS + UNLOAD_SERVO_HOME_MS);
    assert(context.state == UNLOAD_LOGIC_IDLE);
    assert(UnloadLogic_GetServoOutput(&context) == UNLOAD_SERVO_OUTPUT_DISABLE);
    assert(UnloadLogic_GetPendingResult(&context, &result) != 0U);
    assert(result.type == APP_UNLOAD_RESULT_COMPLETE);
    assert(result.inhibit_generation == command.inhibit_generation);
    assert(result.request_id == command.request_id);
    assert(result.job_id == 101U);
    assert(result.route_id == UART_LINETRACER_ROUTE_B);
    assert(result.error_code == UART_ERROR_NONE);

    UnloadLogic_AcknowledgeResult(&context);
    assert(UnloadLogic_GetPendingResult(&context, &result) == 0U);
}

void TestLoadStateDoesNotBlockCompletion() {
    unload_logic_context_t context{};
    app_unload_result_t result{};
    const auto command = MakeStart(102U, UART_LINETRACER_ROUTE_C, 100U);

    UnloadLogic_Init(&context, 0U);
    assert(UnloadLogic_Start(&context, &command, 100U) != 0U);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS);
    assert(context.state == UNLOAD_LOGIC_MOVING_HOME);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS + UNLOAD_SERVO_HOME_MS);
    assert(UnloadLogic_GetPendingResult(&context, &result) != 0U);
    assert(result.type == APP_UNLOAD_RESULT_COMPLETE);
}

void TestServoCycleHandlesTickWraparound() {
    unload_logic_context_t context{};
    app_unload_result_t result{};
    const std::uint32_t started_at_ms = UINT32_MAX - UNLOAD_SERVO_DEPLOY_MS + 1U;
    const auto command = MakeStart(106U, UART_LINETRACER_ROUTE_A, started_at_ms);

    UnloadLogic_Init(&context, started_at_ms);
    assert(UnloadLogic_Start(&context, &command, started_at_ms) != 0U);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 0U);
    assert(context.state == UNLOAD_LOGIC_MOVING_HOME);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, UNLOAD_SERVO_HOME_MS);
    assert(context.state == UNLOAD_LOGIC_IDLE);
    assert(UnloadLogic_GetPendingResult(&context, &result) != 0U);
    assert(result.type == APP_UNLOAD_RESULT_COMPLETE);
}

void TestTimeoutAndAbortAreTerminal() {
    unload_logic_context_t context{};
    app_unload_result_t result{};
    auto command = MakeStart(103U, UART_LINETRACER_ROUTE_A, 100U);

    UnloadLogic_Init(&context, 0U);
    assert(UnloadLogic_Start(&context, &command, 100U) != 0U);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_OPERATION_TIMEOUT_MS);
    assert(UnloadLogic_GetPendingResult(&context, &result) != 0U);
    assert(result.type == APP_UNLOAD_RESULT_TIMEOUT);
    assert(result.error_code == UART_ERROR_TIMEOUT);
    assert(UnloadLogic_Start(&context, &command, 5200U) == 0U);
    UnloadLogic_AcknowledgeResult(&context);
    assert(UnloadLogic_Start(&context, &command, 5300U) == 0U);

    UnloadLogic_Reset(&context, 6000U);
    command = MakeStart(104U, UART_LINETRACER_ROUTE_B, 6100U);
    assert(UnloadLogic_Start(&context, &command, 6100U) != 0U);
    UnloadLogic_Abort(&context, 6200U, UART_ERROR_BUSY);
    assert(UnloadLogic_GetPendingResult(&context, &result) != 0U);
    assert(result.type == APP_UNLOAD_RESULT_ABORTED);
    assert(result.error_code == UART_ERROR_BUSY);
}

void TestRejectsConcurrentOrInvalidStart() {
    unload_logic_context_t context{};
    const auto valid = MakeStart(105U, UART_LINETRACER_ROUTE_A, 100U);
    auto invalid = MakeStart(0U, UART_LINETRACER_ROUTE_A, 100U);

    UnloadLogic_Init(&context, 0U);
    assert(UnloadLogic_Start(&context, &invalid, 100U) == 0U);
    assert(UnloadLogic_Start(&context, &valid, 100U) != 0U);
    assert(UnloadLogic_Start(&context, &valid, 110U) == 0U);
}

}  // namespace

int main() {
    TestCompletionAfterTimedServoCycle();
    TestLoadStateDoesNotBlockCompletion();
    TestServoCycleHandlesTickWraparound();
    TestTimeoutAndAbortAreTerminal();
    TestRejectsConcurrentOrInvalidStart();
    return 0;
}
