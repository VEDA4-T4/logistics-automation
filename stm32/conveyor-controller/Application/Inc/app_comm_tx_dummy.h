#ifndef APP_COMM_TX_DUMMY_H
#define APP_COMM_TX_DUMMY_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * CommTxTask 1단계 dummy 송신기
 * ============================================================================
 *
 * 개발 플랜 1단계(CommTxTask 단독 시험) 전용 코드다.
 * SensorTask/HealthTask 구현이 시작되면 제거한다.
 *
 * 검증 시나리오:
 *   1. 주기 송신: 200ms마다 SENSOR_STATUS를 투입/분류 채널에 번갈아 송신
 *   2. 포화·우선순위 시험: 주기적으로 스케줄러를 잠근 채 normal 메시지를
 *      큐 깊이보다 많이 밀어 넣고(포화 정책 검증) 마지막에 urgent 메시지를
 *      넣는다. 수신 측에서 urgent가 같은 burst의 normal보다 먼저 도착하면
 *      우선순위가 검증된다.
 *
 * dummy EVENT ID (tools/comm_tx_rx_check.c와 값을 맞춘다):
 *
 *   APP_EVENT_DUMMY_BURST  payload: [0]=event_id [1]=burst_id [2]=index
 *   APP_EVENT_DUMMY_URGENT payload: [0]=event_id [1]=burst_id
 */
#define APP_EVENT_DUMMY_BURST 0x02U
#define APP_EVENT_DUMMY_URGENT 0x03U

/* defaultTask에서 호출하며 반환하지 않는다. */
void CommTxDummy_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_COMM_TX_DUMMY_H */
