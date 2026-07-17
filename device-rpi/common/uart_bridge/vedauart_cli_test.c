#define _POSIX_C_SOURCE 200809L

/* Manual userspace smoke test for the VEDAUART character device. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "logistics/contracts/uart_codec.h"

#define VEDAUART_DEVICE_PATH "/dev/vedauart"

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s input-start\n"
            "  %s input-stop\n"
            "  %s input-speed <0..100>\n"
            "  %s input-sensor <state:0..2> <distance_mm:0..65535>\n"
            "  %s sorting-start\n"
            "  %s sorting-stop\n"
            "  %s sorting-speed <0..100>\n"
            "  %s sorting-route <cycle_id:0..65535> <destination:1..3>\n"
            "  %s sorting-sensor <sensor:1..3> <state:0..2> "
            "<distance_mm:0..65535>\n"
            "  %s reset-device\n"
            "  %s estop\n",
            program,
            program,
            program,
            program,
            program,
            program,
            program,
            program,
            program,
            program,
            program);
}

static int parse_u32(const char *text, uint32_t maximum, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if ((text == NULL) || (value == NULL) || (text[0] == '\0'))
    {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);

    if ((errno != 0) || (end == text) || (*end != '\0') ||
        (parsed > maximum))
    {
        return -1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static void put_u16_le(uint8_t *payload, uint32_t low_index, uint16_t value)
{
    payload[low_index] = (uint8_t)(value & 0xFFU);
    payload[low_index + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
}

static int build_frame(int argc, char **argv, uart_frame_t *frame)
{
    uint32_t first;
    uint32_t second;
    uint32_t third;

    if ((argc < 2) || (argv == NULL) || (frame == NULL))
    {
        return -1;
    }

    memset(frame, 0, sizeof(*frame));
    frame->version = UART_PROTOCOL_VERSION;
    frame->sequence = 1U;

    if ((strcmp(argv[1], "input-start") == 0) && (argc == 2))
    {
        frame->command = UART_CMD_INPUT_CONVEYOR_START;
    }
    else if ((strcmp(argv[1], "input-stop") == 0) && (argc == 2))
    {
        frame->command = UART_CMD_INPUT_CONVEYOR_STOP;
    }
    else if ((strcmp(argv[1], "input-speed") == 0) && (argc == 3) &&
             (parse_u32(argv[2], UART_INPUT_CONVEYOR_SPEED_MAX, &first) == 0))
    {
        frame->command = UART_CMD_INPUT_CONVEYOR_SET_SPEED;
        frame->length = UART_INPUT_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
        frame->payload[UART_INPUT_CONVEYOR_SPEED_VALUE_INDEX] =
            (uint8_t)first;
    }
    else if ((strcmp(argv[1], "input-sensor") == 0) && (argc == 4) &&
             (parse_u32(argv[2], UART_SENSOR_FAULT, &first) == 0) &&
             (parse_u32(argv[3], UINT16_MAX, &second) == 0))
    {
        frame->command = UART_CMD_SENSOR_STATUS;
        frame->length = UART_SENSOR_STATUS_PAYLOAD_SIZE;
        frame->payload[UART_SENSOR_ID_INDEX] = UART_INPUT_SENSOR_ID_1;
        frame->payload[UART_SENSOR_STATE_INDEX] = (uint8_t)first;
        put_u16_le(frame->payload,
                   UART_SENSOR_DISTANCE_LOW_INDEX,
                   (uint16_t)second);
    }
    else if ((strcmp(argv[1], "sorting-start") == 0) && (argc == 2))
    {
        frame->command = UART_CMD_SORTING_CONVEYOR_START;
    }
    else if ((strcmp(argv[1], "sorting-stop") == 0) && (argc == 2))
    {
        frame->command = UART_CMD_SORTING_CONVEYOR_STOP;
    }
    else if ((strcmp(argv[1], "sorting-speed") == 0) && (argc == 3) &&
             (parse_u32(argv[2], UART_SORTING_CONVEYOR_SPEED_MAX, &first) == 0))
    {
        frame->command = UART_CMD_SORTING_CONVEYOR_SET_SPEED;
        frame->length = UART_SORTING_CONVEYOR_SET_SPEED_PAYLOAD_SIZE;
        frame->payload[UART_SORTING_CONVEYOR_SPEED_VALUE_INDEX] =
            (uint8_t)first;
    }
    else if ((strcmp(argv[1], "sorting-route") == 0) && (argc == 4) &&
             (parse_u32(argv[2], UINT16_MAX, &first) == 0) &&
             (parse_u32(argv[3], UART_SORTING_DESTINATION_MAX, &second) == 0) &&
             (second >= UART_SORTING_DESTINATION_MIN))
    {
        frame->command = UART_CMD_SORTING_ROUTE_ITEM;
        frame->length = UART_SORTING_ROUTE_PAYLOAD_SIZE;
        put_u16_le(frame->payload,
                   UART_SORTING_ROUTE_CYCLE_ID_LOW_INDEX,
                   (uint16_t)first);
        frame->payload[UART_SORTING_ROUTE_DESTINATION_INDEX] =
            (uint8_t)second;
    }
    else if ((strcmp(argv[1], "sorting-sensor") == 0) && (argc == 5) &&
             (parse_u32(argv[2], UART_SORTING_SENSOR_ID_MAX, &first) == 0) &&
             (first >= UART_SORTING_SENSOR_ID_MIN) &&
             (parse_u32(argv[3], UART_SENSOR_FAULT, &second) == 0) &&
             (parse_u32(argv[4], UINT16_MAX, &third) == 0))
    {
        frame->command = UART_CMD_SENSOR_STATUS;
        frame->length = UART_SENSOR_STATUS_PAYLOAD_SIZE;
        frame->payload[UART_SENSOR_ID_INDEX] = (uint8_t)first;
        frame->payload[UART_SENSOR_STATE_INDEX] = (uint8_t)second;
        put_u16_le(frame->payload,
                   UART_SENSOR_DISTANCE_LOW_INDEX,
                   (uint16_t)third);
    }
    else if ((strcmp(argv[1], "reset-device") == 0) && (argc == 2))
    {
        frame->command = UART_CMD_RESET_DEVICE;
    }
    else if ((strcmp(argv[1], "estop") == 0) && (argc == 2))
    {
        frame->command = UART_CMD_EMERGENCY_STOP;
    }
    else
    {
        return -1;
    }

    if (UART_IS_VALID_INPUT_COMMAND(frame->command) != 0U)
    {
        return UART_IS_VALID_INPUT_PAYLOAD(
                   frame->command, frame->payload, frame->length)
                   ? 0
                   : -1;
    }

    if (UART_IS_VALID_SORTING_COMMAND(frame->command) != 0U)
    {
        return UART_IS_VALID_SORTING_PAYLOAD(
                   frame->command, frame->payload, frame->length)
                   ? 0
                   : -1;
    }

    if (frame->command == UART_CMD_SENSOR_STATUS)
    {
        return 0;
    }

    return UART_IS_VALID_COMMAND_PAYLOAD_LENGTH(
               frame->command, frame->length)
               ? 0
               : -1;
}

static int write_all(int descriptor, const uint8_t *data, size_t length)
{
    size_t offset = 0U;

    while (offset < length)
    {
        ssize_t written = write(descriptor, &data[offset], length - offset);

        if (written > 0)
        {
            offset += (size_t)written;
            continue;
        }

        if ((written < 0) && (errno == EINTR))
        {
            continue;
        }

        return -1;
    }

    return 0;
}

static void print_frame(const uint8_t *data, size_t length)
{
    size_t index;

    printf("TX (%zu bytes):", length);
    for (index = 0U; index < length; ++index)
    {
        printf(" %02X", data[index]);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    uart_frame_t frame;
    uint8_t encoded[UART_MAX_FRAME_SIZE];
    size_t encoded_length = 0U;
    uart_codec_result_t codec_result;
    int descriptor;

    if (build_frame(argc, argv, &frame) != 0)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    codec_result = uart_encode_frame(
        &frame, encoded, sizeof(encoded), &encoded_length);
    if (codec_result != UART_CODEC_OK)
    {
        fprintf(stderr, "uart_encode_frame failed: %d\n", codec_result);
        return EXIT_FAILURE;
    }

    descriptor = open(VEDAUART_DEVICE_PATH, O_RDWR);
    if (descriptor < 0)
    {
        fprintf(stderr,
                "open %s failed: %s\n",
                VEDAUART_DEVICE_PATH,
                strerror(errno));
        return EXIT_FAILURE;
    }

    print_frame(encoded, encoded_length);

    if (write_all(descriptor, encoded, encoded_length) != 0)
    {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        close(descriptor);
        return EXIT_FAILURE;
    }

    if (close(descriptor) != 0)
    {
        fprintf(stderr, "close failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("Sent command 0x%02X through %s\n",
           frame.command,
           VEDAUART_DEVICE_PATH);
    return EXIT_SUCCESS;
}
