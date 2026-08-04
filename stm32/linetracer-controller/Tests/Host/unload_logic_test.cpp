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
    command.job_id = job_id;
    command.route_id = route_id;
    return command;
}

void TestCompletionAfterDebouncedLoadOff() {
    unload_logic_context_t context{};
    app_unload_result_t result{};
    const auto command = MakeStart(101U, UART_LINETRACER_ROUTE_B, 100U);

    UnloadLogic_Init(&context, 0U);
    assert(UnloadLogic_Start(&context, &command, 100U) != 0U);
    assert(UnloadLogic_GetServoOutput(&context) == UNLOAD_SERVO_OUTPUT_RELEASE);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS - 1U);
    assert(context.state == UNLOAD_LOGIC_MOVING_TO_RELEASE);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS);
    assert(context.state == UNLOAD_LOGIC_WAITING_LOAD_OFF);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_EMPTY, 1000U);
    assert(context.state == UNLOAD_LOGIC_MOVING_HOME);
    assert(UnloadLogic_GetServoOutput(&context) == UNLOAD_SERVO_OUTPUT_HOME);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_EMPTY, 1000U + UNLOAD_SERVO_HOME_MS);
    assert(context.state == UNLOAD_LOGIC_IDLE);
    assert(UnloadLogic_GetServoOutput(&context) == UNLOAD_SERVO_OUTPUT_DISABLE);
    assert(UnloadLogic_GetPendingResult(&context, &result) != 0U);
    assert(result.type == APP_UNLOAD_RESULT_COMPLETE);
    assert(result.job_id == 101U);
    assert(result.route_id == UART_LINETRACER_ROUTE_B);
    assert(result.error_code == UART_ERROR_NONE);

    UnloadLogic_AcknowledgeResult(&context);
    assert(UnloadLogic_GetPendingResult(&context, &result) == 0U);
}

void TestNonEmptyLoadDoesNotStartHomeMove() {
    unload_logic_context_t context{};
    const auto command = MakeStart(102U, UART_LINETRACER_ROUTE_C, 100U);

    UnloadLogic_Init(&context, 0U);
    assert(UnloadLogic_Start(&context, &command, 100U) != 0U);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 100U + UNLOAD_SERVO_DEPLOY_MS);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_UNLOADING, 1000U);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 1100U);
    assert(context.state == UNLOAD_LOGIC_WAITING_LOAD_OFF);
}

void TestPreexistingEmptyLoadCannotComplete() {
    unload_logic_context_t context{};
    app_unload_result_t result{};
    const auto command = MakeStart(107U, UART_LINETRACER_ROUTE_A, 100U);

    UnloadLogic_Init(&context, 0U);
    assert(UnloadLogic_Start(&context, &command, 100U) != 0U);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_EMPTY, 100U + UNLOAD_SERVO_DEPLOY_MS);
    assert(context.state == UNLOAD_LOGIC_WAITING_LOAD_OFF);
    assert(context.load_present_seen == 0U);
    assert(UnloadLogic_GetPendingResult(&context, &result) == 0U);

    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_EMPTY, 100U + UNLOAD_OPERATION_TIMEOUT_MS);
    assert(UnloadLogic_GetPendingResult(&context, &result) != 0U);
    assert(result.type == APP_UNLOAD_RESULT_TIMEOUT);
}

void TestLoadOffHandlesTickWraparound() {
    unload_logic_context_t context{};
    const std::uint32_t started_at_ms = UINT32_MAX - UNLOAD_SERVO_DEPLOY_MS + 1U;
    const auto command = MakeStart(106U, UART_LINETRACER_ROUTE_A, started_at_ms);

    UnloadLogic_Init(&context, started_at_ms);
    assert(UnloadLogic_Start(&context, &command, started_at_ms) != 0U);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_PRESENT, 0U);
    assert(context.state == UNLOAD_LOGIC_WAITING_LOAD_OFF);
    UnloadLogic_Update(&context, UART_LINETRACER_LOAD_EMPTY, 0U);
    assert(context.state == UNLOAD_LOGIC_MOVING_HOME);
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
    TestCompletionAfterDebouncedLoadOff();
    TestNonEmptyLoadDoesNotStartHomeMove();
    TestPreexistingEmptyLoadCannotComplete();
    TestLoadOffHandlesTickWraparound();
    TestTimeoutAndAbortAreTerminal();
    TestRejectsConcurrentOrInvalidStart();
    return 0;
}
