#define _POSIX_C_SOURCE 200809L

/* Manual userspace smoke test for the VEDAUART character device. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "logistics/contracts/uart_codec.h"
#include "logistics/contracts/uart_parser.h"

#define VEDAUART_DEVICE_PATH "/dev/vedauart"
#define RESPONSE_BUFFER_SIZE 256U

typedef enum {
    TRANSACTION_OK = 0,
    TRANSACTION_TIMEOUT,
    TRANSACTION_BUSY,
    TRANSACTION_SEQUENCE_CONFLICT,
    TRANSACTION_REMOTE_ERROR,
    TRANSACTION_IO_ERROR
} transaction_result_t;

typedef struct {
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t retries;
    uint32_t timeouts;
    uint32_t busy_responses;
    uint32_t parser_errors;
    uint32_t ignored_frames;
    uint32_t sequence_mismatches;
} roundtrip_stats_t;

typedef enum {
    STEP_NOT_RUN = 0,
    STEP_PASS,
    STEP_FAIL
} step_result_t;

#define INPUT_ROUNDTRIP_STEP_COUNT 6U

static const char* command_name(uint8_t command) {
    switch (command) {
        case UART_CMD_INPUT_CONVEYOR_START:
            return "INPUT_CONVEYOR_START";
        case UART_CMD_INPUT_CONVEYOR_STOP:
            return "INPUT_CONVEYOR_STOP";
        case UART_CMD_INPUT_CONVEYOR_SET_SPEED:
            return "INPUT_CONVEYOR_SET_SPEED";
        case UART_CMD_INPUT_CONVEYOR_GET_STATUS:
            return "INPUT_CONVEYOR_GET_STATUS";
        case UART_CMD_INPUT_CONTROL_RESET:
            return "INPUT_CONTROL_RESET";
        case UART_CMD_SORTING_ROUTE_ITEM:
            return "SORTING_ROUTE_ITEM";
        case UART_CMD_SORTING_GET_STATUS:
            return "SORTING_GET_STATUS";
        case UART_CMD_SORTING_CANCEL:
            return "SORTING_CANCEL";
        case UART_CMD_SORTING_RESET:
            return "SORTING_RESET";
        case UART_CMD_SORTING_CONVEYOR_START:
            return "SORTING_CONVEYOR_START";
        case UART_CMD_SORTING_CONVEYOR_STOP:
            return "SORTING_CONVEYOR_STOP";
        case UART_CMD_SORTING_CONVEYOR_SET_SPEED:
            return "SORTING_CONVEYOR_SET_SPEED";
        case UART_CMD_SORTING_CONVEYOR_GET_STATUS:
            return "SORTING_CONVEYOR_GET_STATUS";
        case UART_CMD_SORTING_RETURN_HOME:
            return "SORTING_RETURN_HOME";
        case UART_CMD_SENSOR_STATUS:
            return "SENSOR_STATUS";
        case UART_CMD_OPERATION_RESULT:
            return "OPERATION_RESULT";
        case UART_CMD_RESPONSE:
            return "RESPONSE";
        case UART_CMD_RESET_DEVICE:
            return "RESET_DEVICE";
        case UART_CMD_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN_COMMAND";
    }
}

static const char* status_name(uint8_t status) {
    switch (status) {
        case UART_STATUS_ACK:
            return "ACK";
        case UART_STATUS_NACK:
            return "NACK";
        case UART_STATUS_BUSY:
            return "BUSY";
        case UART_STATUS_SUCCESS:
            return "SUCCESS";
        case UART_STATUS_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN_STATUS";
    }
}

static const char* error_name(uint8_t error) {
    switch (error) {
        case UART_ERROR_NONE:
            return "NONE";
        case UART_ERROR_INVALID_VERSION:
            return "INVALID_VERSION";
        case UART_ERROR_INVALID_COMMAND:
            return "INVALID_COMMAND";
        case UART_ERROR_INVALID_LENGTH:
            return "INVALID_LENGTH";
        case UART_ERROR_CRC_MISMATCH:
            return "CRC_MISMATCH";
        case UART_ERROR_SEQUENCE:
            return "SEQUENCE";
        case UART_ERROR_TIMEOUT:
            return "TIMEOUT";
        case UART_ERROR_BUSY:
            return "BUSY";
        case UART_ERROR_SENSOR:
            return "SENSOR";
        case UART_ERROR_MOTOR:
            return "MOTOR";
        case UART_ERROR_SERVO:
            return "SERVO";
        case UART_ERROR_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        case UART_ERROR_UNSUPPORTED_COMMAND:
            return "UNSUPPORTED_COMMAND";
        case UART_ERROR_INVALID_PAYLOAD:
            return "INVALID_PAYLOAD";
        case UART_ERROR_INTERNAL:
            return "INTERNAL";
        default:
            return "UNKNOWN_ERROR";
    }
}

static const char* conveyor_state_name(uint8_t state) {
    switch (state) {
        case UART_INPUT_CONVEYOR_STOPPED:
            return "STOPPED";
        case UART_INPUT_CONVEYOR_RUNNING:
            return "RUNNING";
        case UART_INPUT_CONVEYOR_FAULT:
            return "FAULT";
        default:
            return "UNKNOWN_STATE";
    }
}

static const char* transaction_result_name(transaction_result_t result) {
    switch (result) {
        case TRANSACTION_OK:
            return "SUCCESS";
        case TRANSACTION_TIMEOUT:
            return "TIMEOUT";
        case TRANSACTION_BUSY:
            return "BUSY";
        case TRANSACTION_SEQUENCE_CONFLICT:
            return "SEQUENCE_CONFLICT";
        case TRANSACTION_REMOTE_ERROR:
            return "REMOTE_ERROR";
        case TRANSACTION_IO_ERROR:
            return "IO_ERROR";
        default:
            return "UNKNOWN_RESULT";
    }
}

static const char* step_result_name(step_result_t result) {
    switch (result) {
        case STEP_PASS:
            return "PASS";
        case STEP_FAIL:
            return "FAIL";
        case STEP_NOT_RUN:
        default:
            return "NOT RUN";
    }
}

static void print_usage(const char* program) {
    fprintf(stderr,
            "Usage:\n"
            "  %s input-start\n"
            "  %s input-stop\n"
            "  %s input-speed <0..100>\n"
            "  %s input-roundtrip <speed:1..100> [start_sequence:0..255]\n"
            "  %s sorting-start\n"
            "  %s sorting-stop\n"
            "  %s sorting-speed <0..100>\n"
            "  %s sorting-conveyor-status\n"
            "  %s sorting-route <cycle_id:1..65535> <destination:1..3>\n"
            "  %s sorting-return-home <cycle_id:1..65535>\n"
            "  %s sorting-cancel <cycle_id:1..65535>\n"
            "  %s sorting-status\n"
            "  %s sorting-reset\n"
            "  %s reset-device\n"
            "  %s estop\n",
            program, program, program, program, program, program, program, program, program, program, program, program,
            program, program, program);
}

static int parse_u32(const char* text, uint32_t maximum, uint32_t* value) {
    char* end = NULL;
    unsigned long parsed;

    if ((text == NULL) || (value == NULL) || (text[0] == '\0')) {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);

    if ((errno != 0) || (end == text) || (*end != '\0') || (parsed > maximum)) {
        return -1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static void put_u16_le(uint8_t* payload, uint32_t low_index, uint16_t value) {
    payload[low_index] = (uint8_t)(value & 0xFFU);
    payload[low_index + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
}

static int build_frame(int argc, char** argv, uart_frame_t* frame) {
    uint32_t first;
    uint32_t second;

    if ((argc < 2) || (argv == NULL) || (frame == NULL)) {
        return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->version = UART_PROTOCOL_VERSION;
    frame->sequence = 1U;

    if ((strcmp(argv[1], "input-start") == 0) && (argc == 2)) {
        frame->command = UART_CMD_INPUT_CONVEYOR_START;
    } else if ((strcmp(argv[1], "input-stop") == 0) && (argc == 2)) {
        frame->command = UART_CMD_INPUT_CONVEYOR_STOP;
    } else if ((strcmp(argv[1], "input-speed") == 0) && (argc == 3) &&
               (parse_u32(argv[2], UART_INPUT_CONVEYOR_SPEED_MAX, &first) == 0)) {
        frame->command = UART_CMD_INPUT_CONVEYOR_SET_SPEED;
        frame->length = UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
        frame->payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX] = (uint8_t)first;
    } else if ((strcmp(argv[1], "sorting-start") == 0) && (argc == 2)) {
        frame->command = UART_CMD_SORTING_CONVEYOR_START;
    } else if ((strcmp(argv[1], "sorting-stop") == 0) && (argc == 2)) {
        frame->command = UART_CMD_SORTING_CONVEYOR_STOP;
    } else if ((strcmp(argv[1], "sorting-speed") == 0) && (argc == 3) &&
               (parse_u32(argv[2], UART_SORTING_CONVEYOR_SPEED_MAX, &first) == 0)) {
        frame->command = UART_CMD_SORTING_CONVEYOR_SET_SPEED;
        frame->length = UART_SORTING_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
        frame->payload[UART_SORTING_CONVEYOR_SPEED_VALUE_INDEX] = (uint8_t)first;
    } else if ((strcmp(argv[1], "sorting-conveyor-status") == 0) && (argc == 2)) {
        frame->command = UART_CMD_SORTING_CONVEYOR_GET_STATUS;
    } else if ((strcmp(argv[1], "sorting-route") == 0) && (argc == 4) &&
               (parse_u32(argv[2], UINT16_MAX, &first) == 0) &&
               (first >= UART_SORTING_CYCLE_ID_MIN) &&
               (parse_u32(argv[3], UART_SORTING_DESTINATION_MAX, &second) == 0) &&
               (second >= UART_SORTING_DESTINATION_MIN)) {
        frame->command = UART_CMD_SORTING_ROUTE_ITEM;
        frame->length = UART_SORTING_ROUTE_PAYLOAD_SIZE;
        put_u16_le(frame->payload, UART_SORTING_ROUTE_CYCLE_ID_LOW_INDEX, (uint16_t)first);
        frame->payload[UART_SORTING_ROUTE_DESTINATION_INDEX] = (uint8_t)second;
    } else if ((strcmp(argv[1], "sorting-return-home") == 0) && (argc == 3) &&
               (parse_u32(argv[2], UINT16_MAX, &first) == 0) && (first >= UART_SORTING_CYCLE_ID_MIN)) {
        frame->command = UART_CMD_SORTING_RETURN_HOME;
        frame->length = UART_SORTING_RETURN_HOME_PAYLOAD_SIZE;
        put_u16_le(frame->payload, UART_SORTING_RETURN_HOME_CYCLE_ID_LOW_INDEX, (uint16_t)first);
    } else if ((strcmp(argv[1], "sorting-cancel") == 0) && (argc == 3) &&
               (parse_u32(argv[2], UINT16_MAX, &first) == 0) && (first >= UART_SORTING_CYCLE_ID_MIN)) {
        frame->command = UART_CMD_SORTING_CANCEL;
        frame->length = UART_SORTING_CANCEL_PAYLOAD_SIZE;
        put_u16_le(frame->payload, UART_SORTING_CANCEL_CYCLE_ID_LOW_INDEX, (uint16_t)first);
    } else if ((strcmp(argv[1], "sorting-status") == 0) && (argc == 2)) {
        frame->command = UART_CMD_SORTING_GET_STATUS;
    } else if ((strcmp(argv[1], "sorting-reset") == 0) && (argc == 2)) {
        frame->command = UART_CMD_SORTING_RESET;
    } else if ((strcmp(argv[1], "reset-device") == 0) && (argc == 2)) {
        frame->command = UART_CMD_RESET_DEVICE;
    } else if ((strcmp(argv[1], "estop") == 0) && (argc == 2)) {
        frame->command = UART_CMD_EMERGENCY_STOP;
    } else {
        return -1;
    }

    if (UART_IS_VALID_INPUT_COMMAND(frame->command) != 0U) {
        return UART_IS_VALID_INPUT_PAYLOAD(frame->command, frame->payload, frame->length) ? 0 : -1;
    }

    if (UART_IS_VALID_SORTING_COMMAND(frame->command) != 0U) {
        return UART_IS_VALID_SORTING_PAYLOAD(frame->command, frame->payload, frame->length) ? 0 : -1;
    }

    return UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(frame->command, frame->length) ? 0 : -1;
}

static int write_all(int descriptor, const uint8_t* data, size_t length) {
    size_t offset = 0U;

    while (offset < length) {
        ssize_t written = write(descriptor, &data[offset], length - offset);

        if (written > 0) {
            offset += (size_t)written;
            continue;
        }

        if ((written < 0) && (errno == EINTR)) {
            continue;
        }

        return -1;
    }

    return 0;
}

static void print_frame(const char* label, const uint8_t* data, size_t length) {
    size_t index;

    printf("  %-12s (%zu bytes):", label, length);
    for (index = 0U; index < length; ++index) {
        printf(" %02X", data[index]);
    }
    putchar('\n');
}

static void print_step_header(uint32_t step, const char* title, const uart_frame_t* request) {
    printf("\n============================================================\n");
    printf("[STEP %u/%u] %s\n", step, INPUT_ROUNDTRIP_STEP_COUNT, title);
    printf("------------------------------------------------------------\n");
    printf("  Request      : %s (0x%02X)\n", command_name(request->command), request->command);
    printf("  Sequence     : %u\n", request->sequence);
    if (request->length == 0U) {
        printf("  Payload      : none\n");
    } else if ((request->command == UART_CMD_INPUT_CONVEYOR_SET_SPEED) &&
               (request->length == UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE)) {
        printf("  Payload      : speed=%u%%\n", request->payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX]);
    } else {
        printf("  Payload size : %u byte(s)\n", request->length);
    }
}

static void print_transaction_result(transaction_result_t result) {
    printf("  Transaction  : %s\n", transaction_result_name(result));
}

static int64_t monotonic_milliseconds(void) {
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return -1;
    }

    return ((int64_t)timestamp.tv_sec * 1000) + ((int64_t)timestamp.tv_nsec / 1000000);
}

static uint8_t default_sequence(void) {
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 1U;
    }

    return (uint8_t)(((uint32_t)timestamp.tv_nsec ^ (uint32_t)getpid()) & 0xFFU);
}

static int sleep_milliseconds(uint32_t milliseconds) {
    struct timespec remaining;

    remaining.tv_sec = (time_t)(milliseconds / 1000U);
    remaining.tv_nsec = (long)((milliseconds % 1000U) * 1000000UL);

    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return 0;
}

static void initialize_frame(uart_frame_t* frame, uint8_t sequence, uint8_t command) {
    memset(frame, 0, sizeof(*frame));
    frame->version = UART_PROTOCOL_VERSION;
    frame->sequence = sequence;
    frame->command = command;
}

static int send_frame(int descriptor, const uart_frame_t* frame, roundtrip_stats_t* stats) {
    uint8_t encoded[UART_MAX_FRAME_SIZE];
    size_t encodedLength = 0U;
    uart_codec_result_t result;

    result = uart_encode_frame(frame, encoded, sizeof(encoded), &encodedLength);
    if (result != UART_CODEC_OK) {
        fprintf(stderr, "uart_encode_frame failed: %d\n", result);
        return -1;
    }

    print_frame("TX FRAME", encoded, encodedLength);
    fflush(stdout);

    if (write_all(descriptor, encoded, encodedLength) != 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        return -1;
    }

    stats->tx_frames++;

    return 0;
}

static void print_received_response(const uart_frame_t* request, const uart_frame_t* response, uint8_t status,
                                    uint8_t error) {
    uint8_t encoded[UART_MAX_FRAME_SIZE];
    size_t encodedLength = 0U;

    if (uart_encode_frame(response, encoded, sizeof(encoded), &encodedLength) == UART_CODEC_OK) {
        print_frame("RX FRAME", encoded, encodedLength);
    }

    printf("  Response     : %s (0x%02X)\n", command_name(response->command), response->command);
    printf("  Sequence     : %u (%s)\n", response->sequence,
           (response->sequence == request->sequence) ? "MATCH" : "MISMATCH");
    if (response->command == UART_CMD_RESPONSE) {
        uint8_t originalCommand = response->payload[UART_RESPONSE_COMMAND_INDEX];
        printf("  Original cmd : %s (0x%02X)\n", command_name(originalCommand), originalCommand);
    }
    printf("  Status       : %s (0x%02X)\n", status_name(status), status);
    printf("  Error        : %s (0x%02X)\n", error_name(error), error);
}

static transaction_result_t classify_response(const uart_frame_t* request, const uart_frame_t* response) {
    uint8_t status;
    uint8_t error;

    if (response->command == UART_CMD_OPERATION_RESULT) {
        if (response->length != UART_OPERATION_RESULT_PAYLOAD_SIZE) {
            fprintf(stderr, "invalid OPERATION_RESULT payload length: %u\n", response->length);
            return TRANSACTION_REMOTE_ERROR;
        }

        status = response->payload[UART_OPERATION_RESULT_STATUS_INDEX];
        error = response->payload[UART_OPERATION_RESULT_ERROR_INDEX];
    } else if (response->command == UART_CMD_RESPONSE) {
        if (response->length < UART_RESPONSE_HEADER_SIZE) {
            fprintf(stderr, "invalid RESPONSE payload length: %u\n", response->length);
            return TRANSACTION_REMOTE_ERROR;
        }

        if (response->payload[UART_RESPONSE_COMMAND_INDEX] != request->command) {
            return TRANSACTION_REMOTE_ERROR;
        }

        status = response->payload[UART_RESPONSE_STATUS_INDEX];
        error = response->payload[UART_RESPONSE_ERROR_INDEX];
    } else {
        return TRANSACTION_REMOTE_ERROR;
    }

    print_received_response(request, response, status, error);

    if ((status == UART_STATUS_SUCCESS) && (error == UART_ERROR_NONE)) {
        return TRANSACTION_OK;
    }

    if ((status == UART_STATUS_BUSY) || (error == UART_ERROR_BUSY)) {
        return TRANSACTION_BUSY;
    }

    if (error == UART_ERROR_SEQUENCE) {
        return TRANSACTION_SEQUENCE_CONFLICT;
    }

    return TRANSACTION_REMOTE_ERROR;
}

static transaction_result_t wait_for_response(int descriptor, uart_parser_t* parser, const uart_frame_t* request,
                                              uart_frame_t* matchedResponse, roundtrip_stats_t* stats) {
    int64_t deadline;
    uint8_t buffer[RESPONSE_BUFFER_SIZE];

    deadline = monotonic_milliseconds();
    if (deadline < 0) {
        return TRANSACTION_IO_ERROR;
    }
    deadline += UART_ACK_TIMEOUT_MS;

    for (;;) {
        int64_t now = monotonic_milliseconds();
        int timeout;
        struct pollfd pollDescriptor;
        int pollResult;
        ssize_t bytesRead;

        if (now < 0) {
            return TRANSACTION_IO_ERROR;
        }
        if (now >= deadline) {
            uart_parser_reset(parser);
            return TRANSACTION_TIMEOUT;
        }

        timeout = (int)(deadline - now);
        pollDescriptor.fd = descriptor;
        pollDescriptor.events = POLLIN;
        pollDescriptor.revents = 0;
        pollResult = poll(&pollDescriptor, 1U, timeout);

        if (pollResult == 0) {
            uart_parser_reset(parser);
            return TRANSACTION_TIMEOUT;
        }
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            return TRANSACTION_IO_ERROR;
        }
        if ((pollDescriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "UART device poll error: 0x%X\n", pollDescriptor.revents);
            return TRANSACTION_IO_ERROR;
        }
        if ((pollDescriptor.revents & POLLIN) == 0) {
            continue;
        }

        bytesRead = read(descriptor, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }

            fprintf(stderr, "read failed: %s\n", strerror(errno));
            return TRANSACTION_IO_ERROR;
        }
        if (bytesRead == 0) {
            continue;
        }

        for (ssize_t index = 0; index < bytesRead; index++) {
            uart_frame_t response;
            uart_parser_result_t parserResult = uart_parser_feed(parser, buffer[index], &response);

            if (parserResult == UART_PARSER_FRAME_READY) {
                stats->rx_frames++;
                if (response.sequence != request->sequence) {
                    stats->sequence_mismatches++;
                    stats->ignored_frames++;
                    printf("  Ignored RX   : sequence=%u, expected=%u, command=%s (0x%02X)\n", response.sequence,
                           request->sequence, command_name(response.command), response.command);
                    continue;
                }

                if ((response.command != UART_CMD_OPERATION_RESULT) && (response.command != UART_CMD_RESPONSE)) {
                    stats->ignored_frames++;
                    printf("  Ignored RX   : unexpected command=%s (0x%02X)\n", command_name(response.command),
                           response.command);
                    continue;
                }

                *matchedResponse = response;
                return classify_response(request, &response);
            }

            if (parserResult < UART_PARSER_NO_FRAME) {
                stats->parser_errors++;
                fprintf(stderr, "discarded malformed response byte stream: parser result %d\n", parserResult);
            }
        }
    }
}

static transaction_result_t transact(int descriptor, uart_parser_t* parser, const uart_frame_t* request,
                                     uart_frame_t* response, roundtrip_stats_t* stats) {
    transaction_result_t result = TRANSACTION_IO_ERROR;

    for (uint32_t attempt = 0U; attempt <= UART_MAX_RETRY_COUNT; attempt++) {
        if (send_frame(descriptor, request, stats) != 0) {
            return TRANSACTION_IO_ERROR;
        }

        result = wait_for_response(descriptor, parser, request, response, stats);
        if (result == TRANSACTION_TIMEOUT) {
            stats->timeouts++;
        } else if (result == TRANSACTION_BUSY) {
            stats->busy_responses++;
        }
        if (result == TRANSACTION_OK || result == TRANSACTION_SEQUENCE_CONFLICT ||
            result == TRANSACTION_REMOTE_ERROR || result == TRANSACTION_IO_ERROR) {
            return result;
        }

        if (attempt < UART_MAX_RETRY_COUNT) {
            stats->retries++;
            printf("  Retry        : sequence=%u, reason=%s, retry=%u/%u\n", request->sequence,
                   (result == TRANSACTION_BUSY) ? "BUSY" : "TIMEOUT", attempt + 1U, UART_MAX_RETRY_COUNT);
            if (sleep_milliseconds(UART_RETRY_INTERVAL_MS) != 0) {
                fprintf(stderr, "retry delay failed: %s\n", strerror(errno));
                return TRANSACTION_IO_ERROR;
            }
        }
    }

    return result;
}

static int verify_input_status(const uart_frame_t* response, uint8_t expectedState, uint8_t expectedSpeed) {
    uint8_t actualState;
    uint8_t actualSpeed;

    if ((response->command != UART_CMD_RESPONSE) ||
        (response->length != UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE) ||
        (response->payload[UART_RESPONSE_COMMAND_INDEX] != UART_CMD_INPUT_CONVEYOR_GET_STATUS) ||
        (response->payload[UART_RESPONSE_STATUS_INDEX] != UART_STATUS_SUCCESS) ||
        (response->payload[UART_RESPONSE_ERROR_INDEX] != UART_ERROR_NONE)) {
        fprintf(stderr, "  Status data  : invalid GET_STATUS response format\n");
        return -1;
    }

    actualState = response->payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX];
    actualSpeed = response->payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX];
    printf("  Device state : %s (0x%02X)\n", conveyor_state_name(actualState), actualState);
    printf("  Device speed : %u%%\n", actualSpeed);
    printf("  Expected     : state=%s, speed=%u%%\n", conveyor_state_name(expectedState), expectedSpeed);

    if ((actualState != expectedState) || (actualSpeed != expectedSpeed)) {
        fprintf(stderr, "  Status check : MISMATCH\n");
        return -1;
    }

    printf("  Status check : MATCH\n");

    return 0;
}

static int print_roundtrip_summary(const roundtrip_stats_t* stats, const step_result_t* stepResults, uint8_t speed,
                                   uint8_t startSequence) {
    static const char* const stepNames[INPUT_ROUNDTRIP_STEP_COUNT] = {
        "SET_SPEED command",
        "START command",
        "Duplicate START cached-response replay",
        "GET_STATUS reports RUNNING",
        "STOP command",
        "GET_STATUS reports STOPPED",
    };
    uint32_t passedSteps = 0U;

    printf("\n============================================================\n");
    printf("INPUT UART ROUND-TRIP SUMMARY\n");
    printf("============================================================\n");
    printf("  Device       : %s\n", VEDAUART_DEVICE_PATH);
    printf("  Test speed   : %u%%\n", speed);
    printf("  Initial seq  : %u\n", startSequence);
    printf("------------------------------------------------------------\n");
    for (uint32_t index = 0U; index < INPUT_ROUNDTRIP_STEP_COUNT; index++) {
        printf("  [%u] %-39s : %s\n", index + 1U, stepNames[index], step_result_name(stepResults[index]));
        if (stepResults[index] == STEP_PASS) {
            passedSteps++;
        }
    }
    printf("------------------------------------------------------------\n");
    printf("  TX frames            : %u\n", stats->tx_frames);
    printf("  RX valid frames      : %u\n", stats->rx_frames);
    printf("  Retries              : %u\n", stats->retries);
    printf("  Timeouts             : %u\n", stats->timeouts);
    printf("  BUSY responses       : %u\n", stats->busy_responses);
    printf("  Parser errors        : %u\n", stats->parser_errors);
    printf("  Ignored frames       : %u\n", stats->ignored_frames);
    printf("  Sequence mismatches  : %u\n", stats->sequence_mismatches);
    printf("  Passed steps         : %u/%u\n", passedSteps, INPUT_ROUNDTRIP_STEP_COUNT);
    printf("============================================================\n");
    printf("FINAL RESULT: %s\n", (passedSteps == INPUT_ROUNDTRIP_STEP_COUNT) ? "PASS" : "FAIL");
    printf("============================================================\n");

    return (passedSteps == INPUT_ROUNDTRIP_STEP_COUNT) ? 0 : -1;
}

static int run_input_roundtrip(int descriptor, uint8_t speed, uint8_t startSequence) {
    uart_parser_t parser;
    uart_frame_t request;
    uart_frame_t response;
    uart_frame_t firstStartResponse;
    transaction_result_t result;
    roundtrip_stats_t stats;
    step_result_t stepResults[INPUT_ROUNDTRIP_STEP_COUNT];
    uint8_t sequence = startSequence;
    uint8_t startAttempted = 0U;

    uart_parser_init(&parser);
    memset(&stats, 0, sizeof(stats));
    memset(stepResults, 0, sizeof(stepResults));

    printf("============================================================\n");
    printf("INPUT UART ROUND-TRIP TEST\n");
    printf("============================================================\n");
    printf("  Device       : %s\n", VEDAUART_DEVICE_PATH);
    printf("  Speed        : %u%%\n", speed);
    printf("  Initial seq  : %u\n", sequence);
    printf("  ACK timeout  : %u ms\n", UART_ACK_TIMEOUT_MS);
    printf("  Max retries  : %u\n", UART_MAX_RETRY_COUNT);

    for (uint32_t collision = 0U; collision < 256U; collision++) {
        initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_SET_SPEED);
        request.length = UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
        request.payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX] = speed;
        if (collision == 0U) {
            print_step_header(1U, "Set conveyor speed", &request);
        } else {
            printf("  New sequence : %u (retrying SET_SPEED after cache collision)\n", sequence);
        }
        result = transact(descriptor, &parser, &request, &response, &stats);

        if (result != TRANSACTION_SEQUENCE_CONFLICT) {
            break;
        }

        sequence++;
        printf("  Cache result : sequence already belongs to another command\n");
    }

    print_transaction_result(result);
    if (result != TRANSACTION_OK) {
        stepResults[0] = STEP_FAIL;
        printf("  Step result  : FAIL\n");
        goto cleanup;
    }
    stepResults[0] = STEP_PASS;
    printf("  Step result  : PASS\n");

    sequence++;
    initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_START);
    print_step_header(2U, "Start input conveyor", &request);
    startAttempted = 1U;
    result = transact(descriptor, &parser, &request, &firstStartResponse, &stats);
    print_transaction_result(result);
    if (result != TRANSACTION_OK) {
        stepResults[1] = STEP_FAIL;
        printf("  Step result  : FAIL\n");
        goto cleanup;
    }
    stepResults[1] = STEP_PASS;
    printf("  Step result  : PASS\n");

    print_step_header(3U, "Replay duplicate START sequence", &request);
    printf("  Purpose      : verify cached response and prevent duplicate motor execution\n");
    result = transact(descriptor, &parser, &request, &response, &stats);
    print_transaction_result(result);
    if (result != TRANSACTION_OK) {
        stepResults[2] = STEP_FAIL;
        printf("  Cache replay : not received\n");
        printf("  Step result  : FAIL\n");
        goto cleanup;
    }
    if ((response.command != firstStartResponse.command) || (response.length != firstStartResponse.length) ||
        (memcmp(response.payload, firstStartResponse.payload, response.length) != 0)) {
        stepResults[2] = STEP_FAIL;
        printf("  Cache replay : MISMATCH\n");
        printf("  Step result  : FAIL\n");
        goto cleanup;
    }
    stepResults[2] = STEP_PASS;
    printf("  Cache replay : MATCH\n");
    printf("  Step result  : PASS\n");

    sequence++;
    initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_GET_STATUS);
    print_step_header(4U, "Verify RUNNING status", &request);
    result = transact(descriptor, &parser, &request, &response, &stats);
    print_transaction_result(result);
    if ((result != TRANSACTION_OK) || (verify_input_status(&response, UART_INPUT_CONVEYOR_RUNNING, speed) != 0)) {
        stepResults[3] = STEP_FAIL;
        printf("  Step result  : FAIL\n");
        goto cleanup;
    }
    stepResults[3] = STEP_PASS;
    printf("  Step result  : PASS\n");

cleanup:
    if (startAttempted != 0U) {
        sequence++;
        initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_STOP);
        print_step_header(5U, "Stop input conveyor", &request);
        result = transact(descriptor, &parser, &request, &response, &stats);
        print_transaction_result(result);
        if (result != TRANSACTION_OK) {
            stepResults[4] = STEP_FAIL;
            printf("  Step result  : FAIL\n");
        } else {
            stepResults[4] = STEP_PASS;
            printf("  Step result  : PASS\n");

            sequence++;
            initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_GET_STATUS);
            print_step_header(6U, "Verify STOPPED status", &request);
            result = transact(descriptor, &parser, &request, &response, &stats);
            print_transaction_result(result);
            if ((result != TRANSACTION_OK) ||
                (verify_input_status(&response, UART_INPUT_CONVEYOR_STOPPED, speed) != 0)) {
                stepResults[5] = STEP_FAIL;
                printf("  Step result  : FAIL\n");
            } else {
                stepResults[5] = STEP_PASS;
                printf("  Step result  : PASS\n");
            }
        }
    }

    return print_roundtrip_summary(&stats, stepResults, speed, startSequence);
}

int main(int argc, char** argv) {
    uart_frame_t frame;
    uint8_t encoded[UART_MAX_FRAME_SIZE];
    size_t encoded_length = 0U;
    uart_codec_result_t codec_result;
    int descriptor;

    if ((argc >= 2) && (strcmp(argv[1], "input-roundtrip") == 0)) {
        uint32_t speed;
        uint32_t requestedSequence;
        uint8_t sequence;
        int roundtripResult;

        if (((argc != 3) && (argc != 4)) || (parse_u32(argv[2], UART_INPUT_CONVEYOR_SPEED_MAX, &speed) != 0) ||
            (speed == 0U)) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        sequence = default_sequence();
        if (argc == 4) {
            if (parse_u32(argv[3], UINT8_MAX, &requestedSequence) != 0) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            sequence = (uint8_t)requestedSequence;
        }

        descriptor = open(VEDAUART_DEVICE_PATH, O_RDWR);
        if (descriptor < 0) {
            fprintf(stderr, "open %s failed: %s\n", VEDAUART_DEVICE_PATH, strerror(errno));
            return EXIT_FAILURE;
        }

        roundtripResult = run_input_roundtrip(descriptor, (uint8_t)speed, sequence);
        if (close(descriptor) != 0) {
            fprintf(stderr, "close failed: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }

        return (roundtripResult == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (build_frame(argc, argv, &frame) != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    frame.sequence = default_sequence();

    codec_result = uart_encode_frame(&frame, encoded, sizeof(encoded), &encoded_length);
    if (codec_result != UART_CODEC_OK) {
        fprintf(stderr, "uart_encode_frame failed: %d\n", codec_result);
        return EXIT_FAILURE;
    }

    descriptor = open(VEDAUART_DEVICE_PATH, O_RDWR);
    if (descriptor < 0) {
        fprintf(stderr, "open %s failed: %s\n", VEDAUART_DEVICE_PATH, strerror(errno));
        return EXIT_FAILURE;
    }

    print_frame("TX FRAME", encoded, encoded_length);

    if (write_all(descriptor, encoded, encoded_length) != 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        close(descriptor);
        return EXIT_FAILURE;
    }

    if (close(descriptor) != 0) {
        fprintf(stderr, "close failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Sent %s (0x%02X), sequence=%u through %s\n", command_name(frame.command), frame.command, frame.sequence,
           VEDAUART_DEVICE_PATH);
    return EXIT_SUCCESS;
}
