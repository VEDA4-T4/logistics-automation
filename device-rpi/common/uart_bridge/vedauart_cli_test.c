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
            "  %s sorting-route <cycle_id:0..65535> <destination:1..3>\n"
            "  %s sorting-sensor <sensor:1..3> <state:0..2> "
            "<distance_mm:0..65535>\n"
            "  %s reset-device\n"
            "  %s estop\n",
            program, program, program, program, program, program, program, program, program, program, program);
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
    uint32_t third;

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
    } else if ((strcmp(argv[1], "sorting-route") == 0) && (argc == 4) &&
               (parse_u32(argv[2], UINT16_MAX, &first) == 0) &&
               (parse_u32(argv[3], UART_SORTING_DESTINATION_MAX, &second) == 0) &&
               (second >= UART_SORTING_DESTINATION_MIN)) {
        frame->command = UART_CMD_SORTING_ROUTE_ITEM;
        frame->length = UART_SORTING_ROUTE_PAYLOAD_SIZE;
        put_u16_le(frame->payload, UART_SORTING_ROUTE_CYCLE_ID_LOW_INDEX, (uint16_t)first);
        frame->payload[UART_SORTING_ROUTE_DESTINATION_INDEX] = (uint8_t)second;
    } else if ((strcmp(argv[1], "sorting-sensor") == 0) && (argc == 5) &&
               (parse_u32(argv[2], UART_SORTING_SENSOR_ID_MAX, &first) == 0) && (first >= UART_SORTING_SENSOR_ID_MIN) &&
               (parse_u32(argv[3], UART_SENSOR_FAULT, &second) == 0) && (parse_u32(argv[4], UINT16_MAX, &third) == 0)) {
        frame->command = UART_CMD_SENSOR_STATUS;
        frame->length = UART_SENSOR_STATUS_PAYLOAD_SIZE;
        frame->payload[UART_SENSOR_ID_INDEX] = (uint8_t)first;
        frame->payload[UART_SENSOR_STATE_INDEX] = (uint8_t)second;
        put_u16_le(frame->payload, UART_SENSOR_DISTANCE_LOW_INDEX, (uint16_t)third);
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

    if (frame->command == UART_CMD_SENSOR_STATUS) {
        return 0;
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

static void print_frame(const uint8_t* data, size_t length) {
    size_t index;

    printf("TX (%zu bytes):", length);
    for (index = 0U; index < length; ++index) {
        printf(" %02X", data[index]);
    }
    putchar('\n');
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

static int send_frame(int descriptor, const uart_frame_t* frame) {
    uint8_t encoded[UART_MAX_FRAME_SIZE];
    size_t encodedLength = 0U;
    uart_codec_result_t result;

    result = uart_encode_frame(frame, encoded, sizeof(encoded), &encodedLength);
    if (result != UART_CODEC_OK) {
        fprintf(stderr, "uart_encode_frame failed: %d\n", result);
        return -1;
    }

    print_frame(encoded, encodedLength);

    if (write_all(descriptor, encoded, encodedLength) != 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void print_received_response(const uart_frame_t* frame, uint8_t status, uint8_t error) {
    printf("RX: sequence=%u command=0x%02X status=0x%02X error=0x%02X length=%u\n", frame->sequence,
           frame->command, status, error, frame->length);
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

    print_received_response(response, status, error);

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
                                              uart_frame_t* matchedResponse) {
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
                if (response.sequence != request->sequence) {
                    continue;
                }

                if ((response.command != UART_CMD_OPERATION_RESULT) && (response.command != UART_CMD_RESPONSE)) {
                    continue;
                }

                *matchedResponse = response;
                return classify_response(request, &response);
            }

            if (parserResult < UART_PARSER_NO_FRAME) {
                fprintf(stderr, "discarded malformed response byte stream: parser result %d\n", parserResult);
            }
        }
    }
}

static transaction_result_t transact(int descriptor, uart_parser_t* parser, const uart_frame_t* request,
                                     uart_frame_t* response) {
    transaction_result_t result = TRANSACTION_IO_ERROR;

    for (uint32_t attempt = 0U; attempt <= UART_MAX_RETRY_COUNT; attempt++) {
        if (send_frame(descriptor, request) != 0) {
            return TRANSACTION_IO_ERROR;
        }

        result = wait_for_response(descriptor, parser, request, response);
        if (result == TRANSACTION_OK || result == TRANSACTION_SEQUENCE_CONFLICT ||
            result == TRANSACTION_REMOTE_ERROR || result == TRANSACTION_IO_ERROR) {
            return result;
        }

        if (attempt < UART_MAX_RETRY_COUNT) {
            printf("Retrying sequence=%u after %s (%u/%u)\n", request->sequence,
                   (result == TRANSACTION_BUSY) ? "BUSY" : "timeout", attempt + 1U, UART_MAX_RETRY_COUNT);
            if (sleep_milliseconds(UART_RETRY_INTERVAL_MS) != 0) {
                fprintf(stderr, "retry delay failed: %s\n", strerror(errno));
                return TRANSACTION_IO_ERROR;
            }
        }
    }

    return result;
}

static int verify_input_status(const uart_frame_t* response, uint8_t expectedState, uint8_t expectedSpeed) {
    if ((response->command != UART_CMD_RESPONSE) ||
        (response->length != UART_INPUT_CONVEYOR_STATUS_PAYLOAD_SIZE) ||
        (response->payload[UART_RESPONSE_COMMAND_INDEX] != UART_CMD_INPUT_CONVEYOR_GET_STATUS) ||
        (response->payload[UART_RESPONSE_STATUS_INDEX] != UART_STATUS_SUCCESS) ||
        (response->payload[UART_RESPONSE_ERROR_INDEX] != UART_ERROR_NONE) ||
        (response->payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX] != expectedState) ||
        (response->payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX] != expectedSpeed)) {
        fprintf(stderr, "unexpected input status: state=%u speed=%u\n",
                response->payload[UART_INPUT_CONVEYOR_STATUS_STATE_INDEX],
                response->payload[UART_INPUT_CONVEYOR_STATUS_SPEED_INDEX]);
        return -1;
    }

    return 0;
}

static int run_input_roundtrip(int descriptor, uint8_t speed, uint8_t startSequence) {
    uart_parser_t parser;
    uart_frame_t request;
    uart_frame_t response;
    uart_frame_t firstStartResponse;
    transaction_result_t result;
    uint8_t sequence = startSequence;
    uint8_t startAttempted = 0U;
    int passed = 0;

    uart_parser_init(&parser);
    printf("Starting input round-trip test: speed=%u initial_sequence=%u\n", speed, sequence);

    for (uint32_t collision = 0U; collision < 256U; collision++) {
        initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_SET_SPEED);
        request.length = UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
        request.payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX] = speed;
        result = transact(descriptor, &parser, &request, &response);

        if (result != TRANSACTION_SEQUENCE_CONFLICT) {
            break;
        }

        sequence++;
        printf("Sequence was already cached with another command; trying sequence=%u\n", sequence);
    }

    if (result != TRANSACTION_OK) {
        fprintf(stderr, "SET_SPEED transaction failed: %d\n", result);
        goto cleanup;
    }

    sequence++;
    initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_START);
    startAttempted = 1U;
    result = transact(descriptor, &parser, &request, &firstStartResponse);
    if (result != TRANSACTION_OK) {
        fprintf(stderr, "START transaction failed: %d\n", result);
        goto cleanup;
    }

    printf("Re-sending the same START sequence to verify cached-response replay\n");
    result = transact(descriptor, &parser, &request, &response);
    if ((result != TRANSACTION_OK) || (response.command != firstStartResponse.command) ||
        (response.length != firstStartResponse.length) ||
        (memcmp(response.payload, firstStartResponse.payload, response.length) != 0)) {
        fprintf(stderr, "duplicate START response did not match the cached result\n");
        goto cleanup;
    }

    sequence++;
    initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_GET_STATUS);
    result = transact(descriptor, &parser, &request, &response);
    if ((result != TRANSACTION_OK) ||
        (verify_input_status(&response, UART_INPUT_CONVEYOR_RUNNING, speed) != 0)) {
        fprintf(stderr, "RUNNING status verification failed\n");
        goto cleanup;
    }

    passed = 1;

cleanup:
    if (startAttempted != 0U) {
        sequence++;
        initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_STOP);
        result = transact(descriptor, &parser, &request, &response);
        if (result != TRANSACTION_OK) {
            fprintf(stderr, "STOP cleanup transaction failed: %d\n", result);
            passed = 0;
        } else {
            sequence++;
            initialize_frame(&request, sequence, UART_CMD_INPUT_CONVEYOR_GET_STATUS);
            result = transact(descriptor, &parser, &request, &response);
            if ((result != TRANSACTION_OK) ||
                (verify_input_status(&response, UART_INPUT_CONVEYOR_STOPPED, speed) != 0)) {
                fprintf(stderr, "STOPPED status verification failed\n");
                passed = 0;
            }
        }
    }

    printf("Input round-trip test: %s\n", (passed != 0) ? "PASS" : "FAIL");
    return (passed != 0) ? 0 : -1;
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

    print_frame(encoded, encoded_length);

    if (write_all(descriptor, encoded, encoded_length) != 0) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        close(descriptor);
        return EXIT_FAILURE;
    }

    if (close(descriptor) != 0) {
        fprintf(stderr, "close failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Sent command 0x%02X through %s\n", frame.command, VEDAUART_DEVICE_PATH);
    return EXIT_SUCCESS;
}
