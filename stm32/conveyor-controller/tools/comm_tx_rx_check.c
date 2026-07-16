/*
 * ============================================================================
 * CommTxTask 1단계 dummy 송신 수신 검증 프로그램 (Raspberry Pi / Linux)
 * ============================================================================
 *
 * STM32 CommTxTask가 보내는 UART 프레임을 수신해서 다음을 검증한다.
 *
 *   1. 프레임 구조·CRC16-CCITT (shared 계약 파서 재사용)
 *   2. 채널 라우팅: SENSOR_STATUS의 sensor_id가 이 채널 값과 일치
 *   3. heartbeat: EVENT(id 0x01) 주기 1.0±0.5초, 장치 상태·오류·uptime 포함
 *   4. 긴급 우선순위: burst 시험에서 urgent EVENT(id 0x03)가 같은 burst의
 *      normal EVENT(id 0x02)보다 먼저 도착
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
 *       ../../../shared/contracts/uart/uart_crc16.c
 *
 * 사용 예:
 *
 *   투입 Pi (USART1 연결):  ./comm_tx_rx_check /dev/serial0 input
 *   분류 Pi (USART6 연결):  ./comm_tx_rx_check /dev/serial0 sorting
 *   수신 시간 지정(초):     ./comm_tx_rx_check /dev/serial0 input 60
 *
 * 판정 기준 (PASS):
 *   - 프레임 1개 이상 수신
 *   - CRC/버전/명령어/길이 오류 0
 *   - 라우팅 오류(sensor_id 불일치) 0
 *   - heartbeat 2회 이상, 주기 1.0±0.5초 유지
 *   - 긴급 우선순위 위반 0 (burst 시험이 관측된 경우)
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "logistics/contracts/uart_parser.h"
#include "logistics/contracts/uart_protocol.h"

/* dummy 송신기와 약속된 채널별 sensor_id (투입=0, 분류=1) */
#define EXPECTED_SENSOR_ID_INPUT 0U
#define EXPECTED_SENSOR_ID_SORTING 1U

/*
 * 애플리케이션 EVENT ID
 * (펌웨어 Application/Inc/app_comm_tx.h, app_comm_tx_dummy.h와 값을 맞춘다)
 */
#define APP_EVENT_HEARTBEAT 0x01U
#define APP_EVENT_DUMMY_BURST 0x02U
#define APP_EVENT_DUMMY_URGENT 0x03U

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
    uint32_t burst_normal;
    uint32_t burst_urgent;
    uint32_t priority_ok;
    uint32_t priority_fail;
    uint32_t other;
    uint32_t seq_gap;
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

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int open_serial(const char* path) {
    int fd = open(path, O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

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

    /* read(): 최소 0바이트, 0.1초 타임아웃 */
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    tcflush(fd, TCIFLUSH);

    return fd;
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

    uint8_t expected_sensor_id;
    if (strcmp(channel, "input") == 0) {
        expected_sensor_id = EXPECTED_SENSOR_ID_INPUT;
    } else if (strcmp(channel, "sorting") == 0) {
        expected_sensor_id = EXPECTED_SENSOR_ID_SORTING;
    } else {
        fprintf(stderr, "채널은 input 또는 sorting이어야 합니다: %s\n", channel);
        return 2;
    }

    int fd = open_serial(port);
    if (fd < 0) {
        return 2;
    }

    uart_parser_t parser;
    uart_parser_init(&parser);

    rx_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    double last_heartbeat_time = -1.0;
    int have_last_seq = 0;
    uint8_t last_seq = 0U;

    /* 현재 관측 중인 burst의 우선순위 판정 상태 */
    int burst_active_id = -1;
    uint32_t burst_normals_seen = 0U;

    printf("[%s] %s @ 115200 수신 시작 (%.0f초)...\n", channel, port, duration);

    double deadline = monotonic_seconds() + duration;

    while (monotonic_seconds() < deadline) {
        uint8_t buffer[256];
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));

        if (bytes_read < 0) {
            perror("read");
            break;
        }

        if (bytes_read == 0) {
            /* 타임아웃: 부분 프레임 폐기 판정을 위해 경과 시간을 알린다 */
            uart_parser_tick(&parser, 100U);
            continue;
        }

        for (ssize_t i = 0; i < bytes_read; i++) {
            uart_frame_t frame;
            uart_parser_result_t result = uart_parser_feed(&parser, buffer[i], &frame);

            switch (result) {
                case UART_PARSER_NO_FRAME:
                    continue;

                case UART_PARSER_INVALID_SOF:
                    stats.sof_error++;
                    continue;

                case UART_PARSER_INVALID_VERSION:
                    stats.version_error++;
                    continue;

                case UART_PARSER_INVALID_LENGTH:
                    stats.length_error++;
                    continue;

                case UART_PARSER_INVALID_COMMAND:
                    stats.command_error++;
                    continue;

                case UART_PARSER_CRC_ERROR:
                    stats.crc_error++;
                    printf("  CRC 오류 발생\n");
                    continue;

                case UART_PARSER_FRAME_READY:
                    break;

                default:
                    continue;
            }

            /* 완성된 프레임 처리 */
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
                uint16_t distance = (uint16_t)(frame.payload[UART_SENSOR_DISTANCE_LOW_INDEX] |
                                               (frame.payload[UART_SENSOR_DISTANCE_HIGH_INDEX] << 8U));

                if (sensor_id != expected_sensor_id) {
                    stats.wrong_channel++;
                    printf("  라우팅 오류! sensor_id=%u (기대값 %u)\n", sensor_id, expected_sensor_id);
                }

                if (stats.sensor_status % 25U == 1U) {
                    printf("  SENSOR_STATUS: seq=%u sensor_id=%u distance=%umm\n", frame.sequence, sensor_id, distance);
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
                } else if (event_id == APP_EVENT_DUMMY_URGENT) {
                    stats.burst_urgent++;

                    uint8_t burst_id = frame.payload[1];

                    /*
                     * 긴급 우선순위 판정: urgent는 dummy 송신기가 burst의
                     * 마지막에 등록하지만, CommTxTask가 우선 처리하므로
                     * 같은 burst의 normal보다 먼저 도착해야 한다.
                     */
                    if (burst_active_id == (int)burst_id && burst_normals_seen > 0U) {
                        stats.priority_fail++;
                        printf("  우선순위 위반! burst=%u에서 normal %u개가 urgent보다 먼저 도착\n", burst_id,
                               burst_normals_seen);
                    } else {
                        stats.priority_ok++;
                        printf("  긴급 우선 확인: burst=%u urgent가 먼저 도착\n", burst_id);
                    }

                    burst_active_id = (int)burst_id;
                } else if (event_id == APP_EVENT_DUMMY_BURST) {
                    stats.burst_normal++;

                    uint8_t burst_id = frame.payload[1];

                    if (burst_active_id != (int)burst_id) {
                        /* 새 burst의 normal을 urgent보다 먼저 봤다 */
                        burst_active_id = (int)burst_id;
                        burst_normals_seen = 0U;
                    }
                    burst_normals_seen++;
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
    printf("  burst normal       : %u\n", stats.burst_normal);
    printf("  burst urgent       : %u\n", stats.burst_urgent);
    printf("  기타               : %u\n", stats.other);
    printf("  CRC 오류           : %u\n", stats.crc_error);
    printf("  버전 오류          : %u\n", stats.version_error);
    printf("  명령어 오류        : %u\n", stats.command_error);
    printf("  길이 오류          : %u\n", stats.length_error);
    printf("  SOF 재동기화       : %u\n", stats.sof_error);
    printf("  라우팅 오류        : %u\n", stats.wrong_channel);
    printf("  SEQ 끊김           : %u\n", stats.seq_gap);
    printf("  heartbeat 크기 오류: %u\n", stats.heartbeat_bad_size);
    printf("  heartbeat 주기 이탈: %u\n", stats.heartbeat_gap_bad);
    printf("  긴급 우선 확인     : %u\n", stats.priority_ok);
    printf("  긴급 우선 위반     : %u\n", stats.priority_fail);

    int pass = (stats.frames > 0U) && (stats.crc_error == 0U) && (stats.version_error == 0U) &&
               (stats.command_error == 0U) && (stats.length_error == 0U) && (stats.wrong_channel == 0U) &&
               (stats.heartbeat >= 2U) && (stats.heartbeat_bad_size == 0U) && (stats.heartbeat_gap_bad == 0U) &&
               (stats.priority_fail == 0U);

    printf("\n판정: %s\n", pass ? "PASS" : "FAIL");

    if (stats.burst_urgent == 0U) {
        printf("참고: burst 시험이 관측되지 않았습니다. 우선순위 검증에는 %.0f초 이상 수신이 필요합니다.\n", 6.0 * 2.0);
    }

    return pass ? 0 : 1;
}
