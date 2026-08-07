#ifndef APP_COMM_TX_H
#define APP_COMM_TX_H

#include <stdint.h>

#include "logistics/contracts/uart/conveyor_events.h"
#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * CommTxTask 공용 송신 인터페이스
 * ============================================================================
 *
 * CommTxTask는 STM32 UART 송신의 논리적 단일 소유자다.
 * 모든 태스크(Sensor/Safety/Health/Control)는 CommTx_Send() 또는
 * CommTx_SendUrgent()로 송신을 요청하고, CommTxTask가 메시지의 대상 채널에
 * 따라 투입/분류 Raspberry Pi로 라우팅한다.
 *
 * 우선순위:
 *   긴급(urgent) 큐의 메시지는 일반(normal) 큐보다 항상 먼저 처리된다.
 *   비상정지, 안전 상태, 통신 오류 보고는 CommTx_SendUrgent()를 사용한다.
 *
 * 채널 독립성:
 *   채널별 DMA·timeout·재시도 상태를 분리 관리한다. 한 채널의 송신 중
 *   상태가 다른 채널의 송신을 블로킹하지 않는다.
 *
 * 비블로킹:
 *   두 송신 함수 모두 큐가 가득 차면 기다리지 않고 즉시 실패(드랍)한다.
 *   호출 태스크(센서 측정, 모터 제어)는 송신 지연에 블로킹되지 않는다.
 */

/* 송신 대상 Raspberry Pi 채널 (공정 기준) */
typedef enum {
    COMM_TX_CH_INPUT = 0U,   /* USART1, 투입 Raspberry Pi */
    COMM_TX_CH_SORTING = 1U, /* USART6, 분류 Raspberry Pi */
    COMM_TX_CH_COUNT = 2U
} comm_tx_channel_t;

/* TX Queue 메시지. 일반 이벤트는 자동 SEQUENCE, 응답은 요청 SEQUENCE를 사용한다. */
typedef struct {
    uint8_t channel; /* comm_tx_channel_t */
    uint8_t command; /* uart_command_t 또는 장치별 CMD */
    uint8_t length;  /* payload 길이 (0 ~ UART_MAX_PAYLOAD_SIZE) */
    uint8_t sequence;
    uint8_t use_provided_sequence;
    uint8_t payload[UART_MAX_PAYLOAD_SIZE];
} comm_tx_msg_t;

/*
 * ============================================================================
 * 애플리케이션 EVENT 계약
 * ============================================================================
 *
 * ID, payload layout, validator는 Pi와 함께 쓰는 conveyor_events.h에 정의한다.
 */

/*
 * heartbeat Payload 구조 (UART_CMD_EVENT):
 *
 *   [0] event_id      = APP_EVENT_HEARTBEAT
 *   [1] device_state  (uart_device_state_t)
 *   [2] error_code    (uart_error_t, 현재 오류)
 *   [3] uptime 초, Little-endian uint32
 *   [4] uptime
 *   [5] uptime
 *   [6] uptime
 *   [7] 투입 센서 상태  (uart_sensor_state_t)
 *   [8] 분류 센서 상태  (uart_sensor_state_t)
 */

/* 송신 통계. 디버거 Live Expressions로 관찰한다. */
typedef struct {
    uint32_t enqueued;                     /* 일반 큐 등록 성공 수 */
    uint32_t enqueued_urgent;              /* 긴급 큐 등록 성공 수 */
    uint32_t dropped_queue_full;           /* 일반 큐 가득 참으로 드랍 */
    uint32_t dropped_urgent_queue_full;    /* 긴급 큐 가득 참으로 드랍 */
    uint32_t dropped_invalid;              /* 채널/길이 오류로 드랍 */
    uint32_t dropped_encode_error;         /* 프레임 인코딩 실패로 드랍 */
    uint32_t dropped_ring_full;            /* best-effort heartbeat 링 포화 드랍 */
    uint32_t ring_full_waits;              /* 수락한 메시지가 링 자리를 기다린 횟수 */
    uint32_t dropped_retry_exhausted;      /* DMA 재시도 소진 후 드랍 */
    uint32_t init_failures;                /* TX Queue/QueueSet 초기화 실패 */
    uint32_t tx_error[COMM_TX_CH_COUNT];   /* HAL 송신 오류 수 */
    uint32_t tx_timeout[COMM_TX_CH_COUNT]; /* DMA 송신 timeout 수 */
    uint32_t tx_retry[COMM_TX_CH_COUNT];   /* timeout 후 재시도 수 */
    uint32_t sent[COMM_TX_CH_COUNT];       /* DMA 송신 완료 프레임 수 */
    uint32_t heartbeat_sent;               /* heartbeat 송신 시도 수 */
} comm_tx_stats_t;

/*
 * 일반 송신 요청. 상태 보고, 센서 이벤트 등에 사용한다.
 *
 * 태스크 컨텍스트에서만 호출한다. 큐가 가득 차면 기다리지 않고
 * 즉시 실패하며 통계에 드랍으로 기록된다.
 *
 * 반환: 0 성공, 음수 실패
 */
int32_t CommTx_Send(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length);

/* 요청과 동일한 SEQUENCE를 보존해야 하는 RESPONSE/OPERATION_RESULT 전송. */
int32_t CommTx_SendWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command, const uint8_t* payload,
                                uint8_t length);

/*
 * 긴급 송신 요청. 비상정지, 안전 상태, 통신 오류 보고에 사용한다.
 * 일반 큐에 쌓인 메시지보다 항상 먼저 처리된다.
 */
int32_t CommTx_SendUrgent(comm_tx_channel_t channel, uint8_t command, const uint8_t* payload, uint8_t length);

/* 안전 명령 응답처럼 긴급성과 요청 SEQUENCE 보존이 모두 필요한 전송. */
int32_t CommTx_SendUrgentWithSequence(comm_tx_channel_t channel, uint8_t sequence, uint8_t command,
                                      const uint8_t* payload, uint8_t length);

/*
 * Updates the device status included in both heartbeat channels. CommTxTask
 * owns periodic transmission; callers
 * only update the snapshot.
 */
void CommTx_SetDeviceStatus(uint8_t device_state, uint8_t error_code);

/* Updates only the heartbeat snapshot for the selected process channel. */
void CommTx_SetChannelDeviceStatus(comm_tx_channel_t channel, uint8_t device_state, uint8_t error_code);

/*
 * Updates the per-process sensor state included in heartbeat frames. The
 * process must be COMM_TX_CH_INPUT or
 * COMM_TX_CH_SORTING.
 */
void CommTx_SetSensorState(comm_tx_channel_t process, uint8_t sensor_state);

/* 송신 통계 조회 */
const comm_tx_stats_t* CommTx_GetStats(void);

/* 공용 HAL UART 오류 콜백에서 호출하는 TX 오류 처리 진입점. */
void CommTx_HandleUartError(UART_HandleTypeDef* huart);

/* CommTxTask 시작 및 호스트 단위 테스트에서 사용하는 lifecycle API. */
int32_t CommTx_Init(void);
void CommTx_ProcessOnce(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMM_TX_H */
