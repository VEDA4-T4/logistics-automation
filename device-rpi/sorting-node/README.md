# Sorting Raspberry Pi Node (VEDA-120)

분류 노드는 중앙 서버의 MQTT 제어 명령을 FIFO로 수신하고, STM32 분류 컨트롤러의 UART 명령으로 변환한다.
STM32의 처리 결과와 비동기 장치 상태는 다시 MQTT 응답·상태·이벤트·오류 토픽으로 발행한다.

## 실행

최초 실행 전 예제 설정을 복사하고, 파일 내 주석에 따라 장치 IP와 MQTT broker 정보를 수정한다.

```bash
cp device-rpi/config/sorting-node.ini.example device-rpi/config/sorting-node.ini
nano device-rpi/config/sorting-node.ini
```

실제 `sorting-node.ini`는 배포 환경의 IP 주소와 MQTT 인증 정보를 포함할 수 있으므로 Git에 커밋하지 않는다.

```bash
./logistics_sorting_node [sorting-node.ini] [/dev/vedauart]
```

인자를 생략하면 `device-rpi/config/node.ini`와 `/dev/vedauart`를 사용한다. 다음 환경 변수로도 경로를 지정할
수 있다.

- `LOGISTICS_DEVICE_CONFIG`: 노드 설정 파일
- `LOGISTICS_UART_DEVICE`: UART 장치 노드

## MQTT → UART 명령 계약

| MQTT 메시지/명령 | STM32 UART 명령 | Payload |
|---|---|---|
| `DESTINATION_SET` | `SORTING_ROUTE_ITEM (0x30)` | 로컬 `uint16_t cycle_id` + 목적지 `1..3` |
| `START`, `RESTART` (component 생략) | UART 명령 없음 | 시스템 가동 준비만 확인하고 컨베이어는 정지 유지 |
| `START`, `RESTART` (`component=sorting_conveyor`, `params.speed=1..100` 선택) | `SORTING_CONVEYOR_SET_SPEED (0x36)` 성공 후 `SORTING_CONVEYOR_START (0x34)` | 그리퍼 완료 후 중앙 서버가 발행. 속도 생략 시 직전값/기본값 사용 |
| `STOP` | `SORTING_CONVEYOR_STOP (0x35)` | 없음 |
| `INITIALIZE` | `SORTING_RESET (0x33)` | 없음 |
| `STATUS_REQUEST` | `SORTING_GET_STATUS (0x31)` | 없음 |
| `STATUS_REQUEST`, component=`CONVEYOR` | `SORTING_CONVEYOR_GET_STATUS (0x37)` | 없음 |
| `RECOVERY` (component 생략 또는 `GATE`) | `SORTING_RETURN_HOME (0x38)` | 활성 `cycle_id` |
| `RECOVERY`, component=`SAFETY` | `RESET_DEVICE (0x03)` | 없음, 1회 송신 후 Safety EVENT로 결과 확인 |
| `EMERGENCY_STOP` | `EMERGENCY_STOP (0xF0)` | 없음, 대기 명령 선점 후 1회 송신 |

목적지는 `1`, `2`, `3`을 기본값으로 사용하며 `DEST-01..03`, `A..C`도 같은 값으로 정규화한다. 서버의 UUID
`work_id`는 UART에 직접 넣지 않고, 노드가 활성 작업 동안 로컬 `uint16_t cycle_id`와 1:1로 연결한다.

## UART → MQTT 보고 계약

| STM32 프레임 | MQTT 발행 |
|---|---|
| `OPERATION_RESULT`, `RESPONSE` | `device/{id}/response`의 `COMMAND_RESPONSE` |
| 분류·컨베이어 상태 응답 | `device/{id}/status`의 `DEVICE_STATUS` |
| `CYCLE_COMPLETE` | `device/{id}/event`의 `WORK_COMPLETED` |
| `SENSOR_STATUS` | `device/{id}/event`의 `SENSOR_STATUS(sensorId, measurementStatus, distanceCm)`. `measurementStatus`는 `OK`/`FAULT`(측정 건전성)뿐이며, 상자 도착 판정은 중앙 서버가 거리값으로 내려 `detectionStatus`를 덧붙인다 |
| `DEVICE_STATUS`, heartbeat, Safety·Health 이벤트 | 상태 갱신 및 필요 시 `ERROR_OCCURRED` |
| CRC·Parser 오류, 응답 타임아웃, UART 단절 | `device/{id}/error` 및 UART 오류 상태 |

## 순서·중복·복구 정책

1. MQTT callback은 명령을 최대 64개의 큐에 넣고, E-Stop은 일반 FIFO와 분리해 가장 먼저 꺼낸다. 포화 시 `ERR-COMMAND-QUEUE-FULL` 거부 응답을 즉시 발행한다.
2. 메인 루프는 STM32 응답 대기 중이 아닐 때 한 명령만 꺼낸다.
3. 일반 명령은 응답이 없으면 같은 UART frame과 sequence를 최대 3회 재전송한다. E-Stop과 Safety Reset은 비동기 Safety 이벤트를 내는 명령이므로 1회만 송신한다.
4. 성공·실패 응답 또는 최종 타임아웃 뒤에만 다음 MQTT 명령을 처리한다.
5. 같은 MQTT `message_id`가 다시 오면 모터 명령은 반복하지 않고 request ID별 최종 응답 캐시를 재발행한다.
6. 활성 `work_id`와 목적지가 같은 재요청은 UART 동작을 반복하지 않고 `DUPLICATED`로 응답한다.
7. 다른 작업이 활성 상태이면 새 분류 요청을 `ACTIVE_CYCLE_CONFLICT`로 거부한다.
8. 최종 응답 타임아웃이 발생하면 새 명령을 처리하기 전에 STM32 상태를 조회한다. 분류 명령의 결과가
   불확실한 동안에는 해당 work/cycle 연결을 보존하여 같은 물품의 모터 동작이 중복되지 않게 한다.
9. UART가 끊기면 오류 상태를 발행하고 2초마다 재연결한다. 재연결 직후 STM32 상태를 조회하여 노드 상태를
   다시 동기화한다. cycle ID 또는 목적지가 기존 서버 작업 매핑과 다르면 작업 ID를 제거하고 매핑 오류를 보고한다.
10. MQTT가 잠시 끊기면 응답·이벤트·오류를 메모리 outbox에 보존한다. 포화 시 오래된 상태 메시지만 먼저
    제거하고 명령 응답과 공정 이벤트를 우선 보존한다. 센서 거리값은 센서별 최신 측정값 하나로 병합한다.

## 현재 하드웨어 검증 범위

단위 테스트는 UART 가짜 backend를 사용해 명령 변환, 처리 결과, sequence 일치, 재시도, 중복 분류 방지,
cycle 완료 및 오류 보고를 검증한다. 실제 배선 후에는 `/dev/vedauart`와 MQTT broker를 함께 사용하여 목적지
1·2·3의 서보 각도와 모터 동작, 단절·재연결을 최종 확인해야 한다.
