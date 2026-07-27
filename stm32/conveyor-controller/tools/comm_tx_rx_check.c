/*
 * ============================================================================
 * CommTxTask/SensorTask 송신 수신 검증 프로그램 (Raspberry Pi / Linux)
 * ============================================================================
 *
 * STM32 CommTxTask가 보내는 UART 프레임을 수신해서 다음을 검증한다.
 *
 *   1. 프레임 구조·CRC16-CCITT (shared 계약 파서 재사용)
 *   2. 채널 라우팅: 투입 sensor_id=1, 분류 sensor_id=1~3
 *   3. heartbeat: EVENT(id 0x01) 주기 1.0±0.5초, 장치 상태·오류·uptime 포함
 *
 * 프레임 파싱과 CRC 검증은 shared의 계약 구현(uart_parser.c, uart_crc16.c)을
 * 그대로 재사용한다. 이 검증을 통과하면 STM32 송신부와 shared 계약 코드가
 * 서로 맞는다는 것까지 함께 확인된다.
 *
 * 빌드 (저장소를 받은 Raspberry Pi에서, 이 파일이 있는 tools/ 기준):
 *
 *   gcc -O2 -Wall -I../../../shared/include \
 *       -o comm_tx_rx_check \
 *       comm_tx_rx_check.c \
 *       ../../../shared/contracts/uart/uart_parser.c \
 *       ../../../shared/contracts/uart/uart_codec.c \
 *       ../../../shared/contracts/uart/uart_crc16.c
 *
 * 사용 예:
 *
 *   raw tty 직접 수신:        ./comm_tx_rx_check /dev/serial0 input
 *   vedauart 드라이버 경유:   ./comm_tx_rx_check /dev/vedauart input
 *   수신 시간 지정(초):       ./comm_tx_rx_check /dev/serial0 sorting 60
 *
 * /dev/vedauart는 팀의 serdev kernel module(device-rpi/kernel/vedauart)이
 * 제공하는 문자 디바이스다. baudrate는 드라이버가 설정하므로 termios 설정
 * 없이 그대로 읽는다.
 *
 * 판정 기준 (PASS):
 *   - 프레임 1개 이상 수신
 *   - CRC/버전/명령어/길이 오류 0
 *   - 라우팅 오류(sensor_id 불일치) 0
 *   - heartbeat 2회 이상, 주기 1.0±0.5초 유지
 *
 * 초기 동기화:
 *   STM32가 계속 송신 중인 스트림에 프레임 중간부터 접속하면 payload 안의
 *   0xAA를 SOF로 오인해 오류가 1~2회 날 수 있다. 첫 정상 프레임을 받기
 *   전의 파서 오류는 동기화 아티팩트로 보고 판정에서 제외한다.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
#include "logistics/contracts/uart_parser.h"
#include "logistics/contracts/uart_protocol.h"

/* 애플리케이션 EVENT ID (펌웨어 Application/Inc/app_comm_tx.h와 동일) */
#define APP_EVENT_HEARTBEAT 0x01U

/* heartbeat payload 인덱스 (app_comm_tx.h와 동일) */
#define APP_HEARTBEAT_STATE_INDEX 1U
#define APP_HEARTBEAT_ERROR_INDEX 2U
#define APP_HEARTBEAT_UPTIME_INDEX 3U
#define APP_HEARTBEAT_INPUT_SENSOR_INDEX 7U
#define APP_HEARTBEAT_SORTING_SENSOR_INDEX 8U
#define APP_HEARTBEAT_PAYLOAD_SIZE 9U

/* heartbeat 주기 판정 기준 */
#define HEARTBEAT_PERIOD_S 1.0
#define HEARTBEAT_TOLERANCE_S 0.5

/* SafetyTask 안전 EVENT (펌웨어 Application/Inc/safety_task.h와 동일) */
#define APP_EVENT_SAFETY 0x03U

#define APP_SAFETY_EVENT_KIND_INDEX 1U
#define APP_SAFETY_EVENT_CAUSE_INDEX 2U
#define APP_SAFETY_EVENT_TIMESTAMP_INDEX 3U
#define APP_SAFETY_EVENT_RESULT_INDEX 7U
#define APP_SAFETY_EVENT_PAYLOAD_SIZE 8U

/* HealthTask 헬스 EVENT (펌웨어 Application/Inc/health_task.h와 동일) */
#define APP_EVENT_HEALTH 0x04U

#define APP_HEALTH_EVENT_KIND_INDEX 1U
#define APP_HEALTH_EVENT_CAUSE_INDEX 2U
#define APP_HEALTH_EVENT_SENSOR_ID_INDEX 3U
#define APP_HEALTH_EVENT_TIMESTAMP_INDEX 4U
#define APP_HEALTH_EVENT_PAYLOAD_SIZE 8U
#define HEALTH_ISSUE_CAUSE_DEVICE_WIDE 0xFFU
#define HEALTH_ISSUE_SENSOR_ID_NONE 0xFFU

/* 기본 수신 시간 */
#define DEFAULT_DURATION_S 30.0

typedef struct {
    uint32_t frames;
    uint32_t crc_error;
    uint32_t version_error;
    uint32_t command_error;
    uint32_t length_error;
    uint32_t sof_error;
    uint32_t wrong_channel;
    uint32_t sensor_status;
    uint32_t heartbeat;
    uint32_t heartbeat_bad_size;
    uint32_t heartbeat_gap_bad;
    uint32_t other;
    uint32_t seq_gap;
    uint32_t sync_discard;
} rx_stats_t;

static const char* command_name(uint8_t command) {
    switch (command) {
        case UART_CMD_PING:
            return "PING";
        case UART_CMD_GET_STATUS:
            return "GET_STATUS";
        case UART_CMD_RESET_DEVICE:
            return "RESET_DEVICE";
        case UART_CMD_SENSOR_STATUS:
            return "SENSOR_STATUS";
        case UART_CMD_DEVICE_STATUS:
            return "DEVICE_STATUS";
        case UART_CMD_OPERATION_RESULT:
            return "OPERATION_RESULT";
        case UART_CMD_RESPONSE:
            return "RESPONSE";
        case UART_CMD_ACK:
            return "ACK";
        case UART_CMD_EVENT:
            return "EVENT";
        case UART_CMD_EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

static const char* safety_event_kind_name(uint8_t kind) {
    switch (kind) {
        case 1U:
            return "ESTOP_LATCHED";
        case 2U:
            return "RESET_COMPLETE";
        case 3U:
            return "RESET_REJECTED";
        default:
            return "UNKNOWN";
    }
}

static const char* safety_cause_name(uint8_t cause) {
    switch (cause) {
        case 0U:
            return "NONE";
        case 1U:
            return "ESTOP_INPUT_PI";
        case 2U:
            return "ESTOP_SORTING_PI";
        case 3U:
            return "FATAL_ERROR";
        default:
            return "UNKNOWN";
    }
}

static const char* health_issue_kind_name(uint8_t kind) {
    switch (kind) {
        case 1U:
            return "UART_CHANNEL_TIMEOUT";
        case 2U:
            return "QUEUE_OVERFLOW_TRANSIENT";
        case 3U:
            return "SENSOR_STALE";
        default:
            return "UNKNOWN";
    }
}

static const char* health_cause_name(uint8_t cause) {
    if (cause == HEALTH_ISSUE_CAUSE_DEVICE_WIDE) {
        return "DEVICE_WIDE";
    }
    if (cause == 0U) {
        return "INPUT";
    }
    if (cause == 1U) {
        return "SORTING";
    }
    return "UNKNOWN";
}

static const char* safety_reset_result_name(uint8_t result) {
    switch (result) {
        case 0U:
            return "OK";
        case 1U:
            return "INPUT_NOT_READY";
        case 2U:
            return "SORTING_NOT_READY";
        case 3U:
            return "TIMEOUT";
        default:
            return "N/A";
    }
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * 수신 디바이스를 연다.
 *
 * tty(/dev/serial0 등)는 termios로 115200 8N1 raw 모드를 설정한다.
 * vedauart 문자 디바이스(/dev/vedauart)는 tty가 아니고 드라이버가
 * baudrate를 직접 설정하므로 termios를 건너뛴다.
 *
 * 두 경우 모두 O_NONBLOCK으로 열고 수신 대기는 poll()로 한다.
 */
static int open_serial(const char* path) {
    int fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (isatty(fd)) {
        struct termios tio;
        if (tcgetattr(fd, &tio) != 0) {
            perror("tcgetattr");
            close(fd);
            return -1;
        }

        cfmakeraw(&tio);
        cfsetispeed(&tio, B115200);
        cfsetospeed(&tio, B115200);

        /* 8-N-1 */
        tio.c_cflag &= ~(tcflag_t)(PARENB | CSTOPB | CSIZE);
        tio.c_cflag |= CS8 | CLOCAL | CREAD;

        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;

        if (tcsetattr(fd, TCSANOW, &tio) != 0) {
            perror("tcsetattr");
            close(fd);
            return -1;
        }

        tcflush(fd, TCIFLUSH);
    }

    return fd;
}

/*
 * 접속 전에 쌓여 있던 수신 데이터를 버린다.
 *
 * vedauart 드라이버는 64KB 수신 버퍼를 유지하므로, 프로그램 시작 전에
 * STM32가 보낸 옛 데이터가 남아 있을 수 있다. 그대로 파싱하면 buffered
 * 프레임들이 한꺼번에 쏟아져 heartbeat 주기 측정이 왜곡된다.
 */
static void discard_stale_data(int fd) {
    double flush_deadline = monotonic_seconds() + 0.5;

    while (monotonic_seconds() < flush_deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };

        if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
            uint8_t discard[1024];
            (void)read(fd, discard, sizeof(discard));
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "사용법: %s <시리얼 포트> <input|sorting> [수신 시간(초)]\n", argv[0]);
        fprintf(stderr, "예:     %s /dev/serial0 input 30\n", argv[0]);
        return 2;
    }

    const char* port = argv[1];
    const char* channel = argv[2];
    double duration = (argc >= 4) ? atof(argv[3]) : DEFAULT_DURATION_S;

    uint8_t input_channel;
    if (strcmp(channel, "input") == 0) {
        input_channel = 1U;
    } else if (strcmp(channel, "sorting") == 0) {
        input_channel = 0U;
    } else {
        fprintf(stderr, "채널은 input 또는 sorting이어야 합니다: %s\n", channel);
        return 2;
    }

    int fd = open_serial(port);
    if (fd < 0) {
        return 2;
    }

    discard_stale_data(fd);

    uart_parser_t parser;
    uart_parser_init(&parser);

    rx_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    double last_heartbeat_time = -1.0;
    int have_last_seq = 0;
    uint8_t last_seq = 0U;

    /* 첫 정상 프레임 수신 전에는 파서 오류를 동기화 아티팩트로 무시한다 */
    int synced = 0;

    printf("[%s] %s @ 115200 수신 시작 (%.0f초)...\n", channel, port, duration);

    double deadline = monotonic_seconds() + duration;

    while (monotonic_seconds() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int poll_result = poll(&pfd, 1, 100);

        if (poll_result < 0) {
            perror("poll");
            break;
        }

        if (poll_result == 0) {
            /* 타임아웃: 부분 프레임 폐기 판정을 위해 경과 시간을 알린다 */
            uart_parser_tick(&parser, 100U);
            continue;
        }

        uint8_t buffer[256];
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));

        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            perror("read");
            break;
        }

        if (bytes_read == 0) {
            continue;
        }

        for (ssize_t i = 0; i < bytes_read; i++) {
            uart_frame_t frame;
            uart_parser_result_t result = uart_parser_feed(&parser, buffer[i], &frame);

            switch (result) {
                case UART_PARSER_NO_FRAME:
                    continue;

                case UART_PARSER_FRAME_READY:
                    break;

                case UART_PARSER_INVALID_SOF:
                case UART_PARSER_INVALID_VERSION:
                case UART_PARSER_INVALID_LENGTH:
                case UART_PARSER_INVALID_COMMAND:
                case UART_PARSER_CRC_ERROR:
                    if (!synced) {
                        stats.sync_discard++;
                        continue;
                    }

                    if (result == UART_PARSER_INVALID_SOF) {
                        stats.sof_error++;
                    } else if (result == UART_PARSER_INVALID_VERSION) {
                        stats.version_error++;
                    } else if (result == UART_PARSER_INVALID_LENGTH) {
                        stats.length_error++;
                    } else if (result == UART_PARSER_INVALID_COMMAND) {
                        stats.command_error++;
                    } else {
                        stats.crc_error++;
                        printf("  CRC 오류 발생\n");
                    }
                    continue;

                default:
                    continue;
            }

            /* 완성된 프레임 처리 */
            synced = 1;
            stats.frames++;

            /* SEQUENCE 연속성 (드랍/재시도 감지용 통계, 오류 아님) */
            if (have_last_seq && frame.sequence != (uint8_t)(last_seq + 1U)) {
                stats.seq_gap++;
            }
            last_seq = frame.sequence;
            have_last_seq = 1;

            if (frame.command == UART_CMD_SENSOR_STATUS) {
                stats.sensor_status++;

                uint8_t sensor_id = frame.payload[UART_SENSOR_ID_INDEX];
                uint8_t state = frame.payload[UART_SENSOR_STATE_INDEX];
                uint16_t distance_cm =
                    (uint16_t)(frame.payload[UART_SENSOR_DISTANCE_CM_LOW_INDEX] |
                               ((uint16_t)frame.payload[UART_SENSOR_DISTANCE_CM_HIGH_INDEX] << 8U));
                uint8_t sensor_id_is_valid =
                    input_channel != 0U ? uart_input_sensor_id_is_valid(sensor_id)
                                        : uart_sorting_sensor_id_is_valid(sensor_id);

                if (sensor_id_is_valid == 0U) {
                    stats.wrong_channel++;
                    printf("  라우팅 오류! sensor_id=%u (기대값 %s)\n", sensor_id,
                           input_channel != 0U ? "1" : "1~3");
                }

                if (stats.sensor_status % 25U == 1U) {
                    printf("  SENSOR_STATUS: seq=%u sensor_id=%u state=%u distance=%ucm\n", frame.sequence, sensor_id,
                           state, distance_cm);
                }
            } else if (frame.command == UART_CMD_EVENT) {
                uint8_t event_id = frame.payload[UART_EVENT_ID_INDEX];

                if (event_id == APP_EVENT_HEARTBEAT) {
                    stats.heartbeat++;

                    if (frame.length != APP_HEARTBEAT_PAYLOAD_SIZE) {
                        stats.heartbeat_bad_size++;
                        printf("  heartbeat 크기 오류: %u (기대값 %u)\n", frame.length, APP_HEARTBEAT_PAYLOAD_SIZE);
                    } else {
                        uint32_t uptime = (uint32_t)frame.payload[APP_HEARTBEAT_UPTIME_INDEX] |
                                          ((uint32_t)frame.payload[APP_HEARTBEAT_UPTIME_INDEX + 1U] << 8U) |
                                          ((uint32_t)frame.payload[APP_HEARTBEAT_UPTIME_INDEX + 2U] << 16U) |
                                          ((uint32_t)frame.payload[APP_HEARTBEAT_UPTIME_INDEX + 3U] << 24U);

                        if (stats.heartbeat % 10U == 1U) {
                            printf("  heartbeat: state=%u error=%u uptime=%us sensors=[%u,%u]\n",
                                   frame.payload[APP_HEARTBEAT_STATE_INDEX], frame.payload[APP_HEARTBEAT_ERROR_INDEX],
                                   uptime, frame.payload[APP_HEARTBEAT_INPUT_SENSOR_INDEX],
                                   frame.payload[APP_HEARTBEAT_SORTING_SENSOR_INDEX]);
                        }
                    }

                    double now = monotonic_seconds();
                    if (last_heartbeat_time >= 0.0) {
                        double gap = now - last_heartbeat_time;
                        if (gap < HEARTBEAT_PERIOD_S - HEARTBEAT_TOLERANCE_S ||
                            gap > HEARTBEAT_PERIOD_S + HEARTBEAT_TOLERANCE_S) {
                            stats.heartbeat_gap_bad++;
                            printf("  heartbeat 주기 이탈: %.2f초\n", gap);
                        }
                    }
                    last_heartbeat_time = now;
                } else if (event_id == APP_EVENT_SAFETY) {
                    stats.other++;

                    if (frame.length != APP_SAFETY_EVENT_PAYLOAD_SIZE) {
                        printf("  안전 EVENT 크기 오류: %u (기대값 %u)\n", frame.length,
                               APP_SAFETY_EVENT_PAYLOAD_SIZE);
                    } else {
                        uint8_t kind = frame.payload[APP_SAFETY_EVENT_KIND_INDEX];
                        uint8_t cause = frame.payload[APP_SAFETY_EVENT_CAUSE_INDEX];
                        uint8_t result = frame.payload[APP_SAFETY_EVENT_RESULT_INDEX];
                        uint32_t timestamp_ms = (uint32_t)frame.payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX] |
                                                ((uint32_t)frame.payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 1U]
                                                 << 8U) |
                                                ((uint32_t)frame.payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 2U]
                                                 << 16U) |
                                                ((uint32_t)frame.payload[APP_SAFETY_EVENT_TIMESTAMP_INDEX + 3U]
                                                 << 24U);

                        printf("  안전 EVENT: kind=%s(%u) cause=%s(%u) timestamp=%ums result=%s(%u) seq=%u\n",
                               safety_event_kind_name(kind), kind, safety_cause_name(cause), cause, timestamp_ms,
                               safety_reset_result_name(result), result, frame.sequence);
                    }
                } else if (event_id == APP_EVENT_HEALTH) {
                    stats.other++;

                    if (frame.length != APP_HEALTH_EVENT_PAYLOAD_SIZE) {
                        printf("  헬스 EVENT 크기 오류: %u (기대값 %u)\n", frame.length,
                               APP_HEALTH_EVENT_PAYLOAD_SIZE);
                    } else {
                        uint8_t kind = frame.payload[APP_HEALTH_EVENT_KIND_INDEX];
                        uint8_t cause = frame.payload[APP_HEALTH_EVENT_CAUSE_INDEX];
                        uint8_t sensor_id = frame.payload[APP_HEALTH_EVENT_SENSOR_ID_INDEX];
                        uint32_t timestamp_ms = (uint32_t)frame.payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX] |
                                                ((uint32_t)frame.payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX + 1U]
                                                 << 8U) |
                                                ((uint32_t)frame.payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX + 2U]
                                                 << 16U) |
                                                ((uint32_t)frame.payload[APP_HEALTH_EVENT_TIMESTAMP_INDEX + 3U]
                                                 << 24U);

                        if (sensor_id == HEALTH_ISSUE_SENSOR_ID_NONE) {
                            printf("  헬스 EVENT: kind=%s(%u) cause=%s(%u) timestamp=%ums seq=%u\n",
                                   health_issue_kind_name(kind), kind, health_cause_name(cause), cause, timestamp_ms,
                                   frame.sequence);
                        } else {
                            printf("  헬스 EVENT: kind=%s(%u) cause=%s(%u) sensorId=%u timestamp=%ums seq=%u\n",
                                   health_issue_kind_name(kind), kind, health_cause_name(cause), cause, sensor_id,
                                   timestamp_ms, frame.sequence);
                        }
                    }
                } else {
                    stats.other++;
                    printf("  알 수 없는 EVENT id=0x%02X seq=%u\n", event_id, frame.sequence);
                }
            } else {
                stats.other++;
                printf("  기타 프레임: %s seq=%u len=%u\n", command_name(frame.command), frame.sequence, frame.length);
            }
        }
    }

    close(fd);

    printf("\n==================================================\n");
    printf("수신 결과 [%s]\n", channel);
    printf("==================================================\n");
    printf("  총 프레임          : %u\n", stats.frames);
    printf("  SENSOR_STATUS      : %u\n", stats.sensor_status);
    printf("  heartbeat          : %u\n", stats.heartbeat);
    printf("  기타               : %u\n", stats.other);
    printf("  CRC 오류           : %u\n", stats.crc_error);
    printf("  버전 오류          : %u\n", stats.version_error);
    printf("  명령어 오류        : %u\n", stats.command_error);
    printf("  길이 오류          : %u\n", stats.length_error);
    printf("  SOF 재동기화       : %u\n", stats.sof_error);
    printf("  초기 동기화 무시   : %u\n", stats.sync_discard);
    printf("  라우팅 오류        : %u\n", stats.wrong_channel);
    printf("  SEQ 끊김           : %u\n", stats.seq_gap);
    printf("  heartbeat 크기 오류: %u\n", stats.heartbeat_bad_size);
    printf("  heartbeat 주기 이탈: %u\n", stats.heartbeat_gap_bad);

    int pass = (stats.frames > 0U) && (stats.crc_error == 0U) && (stats.version_error == 0U) &&
               (stats.command_error == 0U) && (stats.length_error == 0U) && (stats.wrong_channel == 0U) &&
               (stats.sensor_status > 0U) && (stats.heartbeat >= 2U) && (stats.heartbeat_bad_size == 0U) &&
               (stats.heartbeat_gap_bad == 0U);

    printf("\n판정: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}
