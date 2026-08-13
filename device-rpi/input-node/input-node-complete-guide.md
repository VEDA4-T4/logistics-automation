# 투입 컨베이어 input-node 데몬 완전 해설서

이 문서는 `device-rpi/input-node/`의 투입 컨베이어 중계 데몬을 처음 보는 사람도 이해할 수 있도록
"왜 이렇게 만들었는지"까지 풀어서 설명합니다.

---

## 0부. 이 노드가 하는 일 한 줄 요약

**중앙 서버(MQTT) ↔ 투입 컨베이어 STM32(UART)** 사이의 통역사입니다.

```
Raspberry Pi (중앙 서버)                Raspberry Pi (투입 노드 = 이 데몬)              STM32 (input-controller)
        │                                        │                                          │
        │  MQTT: device/PI-INPUT-01/command       │                                          │
        │ ──────────────────────────────────────▶│  UART frame (SOF..CRC16)                  │
        │   (START / STOP / GET_STATUS /          │ ────────────────────────────────────────▶│  컨베이어 모터 제어
        │    EMERGENCY_STOP ...)                   │                                          │
        │                                        │  UART: RESPONSE / OPERATION_RESULT         │
        │  MQTT: device/PI-INPUT-01/response      │ ◀────────────────────────────────────────│  (명령 결과)
        │ ◀──────────────────────────────────────│                                          │
        │  MQTT: device/PI-INPUT-01/status,event  │  UART: SENSOR_STATUS / DEVICE_STATUS       │
        │ ◀──────────────────────────────────────│ ◀────────────────────────────────────────│  (초음파 센서/장치 상태)
```

완료 조건 3가지를 모두 만족합니다.
1. **공통 통신 규격 준수** — `shared/contracts`의 UART frame·MQTT envelope를 그대로 사용.
2. **명령 timeout·재연결 처리** — UART transact 재시도/timeout, UART·MQTT 양쪽 재연결.
3. **센서·모터 상태 보고** — 초음파 센서·컨베이어 상태를 중앙 서버로 발행.

---

## 1부. 배경지식 — 두 개의 통신 규격

### 1-1. MQTT 쪽 (Pi ↔ 중앙 서버)

이미 `device-rpi/common/`에 완성된 프레임워크가 있습니다. 이 노드는 그걸 **그대로 재사용**합니다.

- `MqttNodeClient` — libmosquitto 래퍼. 접속·재연결(지수 백오프)·Last Will(오프라인 상태 retain)·
  `device/{id}/command` + `system/broadcast/command` 구독·접속 시 온라인상태+장치등록 자동 발행까지 전부 내장.
- `MqttNodeConfig` + `LoadMqttNodeConfig` — `node.ini` 파서.
- `MqttMessageProcessor` / `mqtt_codec.hpp` — JSON envelope(protocolVersion/messageId/messageType/
  sourceId/timestamp/data) 타입 코덱. `ControlCommandPayload`, `CommandResponsePayload`,
  `DeviceStatusPayload`, `ErrorOccurredPayload`, `EmergencyStopPayload` 등 payload별 구조체 + 검증.
- `DeviceStatus` — 연결상태/현재상태/uart_connected/uptime을 담는 스레드 안전 상태 객체.

MQTT 토픽 규칙(`device/{deviceId}/{command|response|status|event|error|heartbeat}`)과 QoS/retain 정책은
`shared/contracts/mqtt/README.md` 참고.

### 1-2. UART 쪽 (Pi ↔ STM32)

`shared/contracts`의 프레임: `SOF | VERSION | SEQUENCE | COMMAND | LENGTH | PAYLOAD | CRC16`.
Pi↔STM32 사이 `/dev/vedauart` 캐릭터 디바이스는 **바이트 스트림만** 주고받고, 프레임 파싱·CRC·ACK·재시도는
유저스페이스(=이 데몬) 몫입니다.

**투입 명령 규격**(`shared/include/logistics/contracts/uart/input_commands.h`):

| 명령 | 값 | payload |
|---|---|---|
| `UART_CMD_INPUT_CONVEYOR_START` | 0x10 | 없음 |
| `UART_CMD_INPUT_CONVEYOR_STOP` | 0x11 | 없음 |
| `UART_CMD_INPUT_CONVEYOR_SET_SPEED` | 0x12 | `[speed]` (1B) |
| `UART_CMD_INPUT_CONVEYOR_GET_STATUS` | 0x13 | 없음 |
| `UART_CMD_INPUT_CONTROL_RESET` | 0x14 | 없음 |
| `UART_CMD_EMERGENCY_STOP` | 0xF0 (공통) | 없음 |

> 이 계약은 투입 컨베이어 벨트 1대만 다루므로 conveyor_id를 전송하지 않는다. STM32
> conveyor-controller 리포의 `input_control.c`가 구현한 이름과 동일하다.

---

## 2부. 핵심 설계 결정 — 왜 이렇게 만들었나

### 2-1. 왜 `UartSession`(linetracer 동료 파일)을 재사용하지 않았나

`feature/linetracer-node` 브랜치에 이미 `UartSession`(프레임 세션 + ACK 대기 + 재시도)이 있습니다.
그런데 **input STM32는 명령에 대해 ACK 프레임(0xE1)을 보내지 않습니다.** 실제
`stm32/conveyor-controller`의 `input_control_task.c`는 명령에 대해:

- `UART_CMD_INPUT_CONVEYOR_GET_STATUS` → **`UART_CMD_RESPONSE`(0xE0)** (상태 데이터 포함)
- 그 외 명령 → **`UART_CMD_OPERATION_RESULT`(0x52)** (status + error)

로, 요청의 **sequence를 그대로 붙여** 응답합니다. (예전 `tools/vedauart_cli_test.c`가 기다리던 것과 동일.)

동료의 `UartSession`은 오직 ACK 프레임에서만 pending 명령을 해제하므로, 그대로 쓰면 RESPONSE/
OPERATION_RESULT가 와도 pending이 안 풀려 매 명령이 불필요하게 3회 재전송 + timeout이 됩니다.

**팀 방침(동료가 만든 파일은 수정하지 않는다)** 에 따라, `UartSession`을 고치는 대신 input 전용
세션(`InputUartSession`)을 별도로 작성했습니다. 검증된 `vedauart_cli_test.c`의 transact 로직을
RESPONSE 모델로 이식한 것입니다.

### 2-2. 계층 분리 — 왜 3개로 나눴나

STM32 HealthTask에서 순수로직/하드웨어를 나눴던 것과 같은 이유로, 하드웨어·OS 의존을 격리해
**하드웨어 없이 호스트에서 단위 테스트**할 수 있게 했습니다.

```
main.cpp (데몬 루프, 시그널, MQTT 배선)        ← 실기기/mosquitto 필요
   │
   ├─ InputNode (순수 로직: MQTT ↔ UART 매핑)   ← 호스트 테스트 가능
   │      │
   │      └─ InputUartSession (transact/재시도)  ← 호스트 테스트 가능 (UartIoBackend fake)
   │             │
   │             └─ UartTransport (/dev/vedauart)  ← 동료 파일, 수정 없이 재사용
```

`UartTransport`의 `UartIoBackend` 순수가상 인터페이스 덕분에, 테스트에서는 가짜 백엔드가 요청 프레임을
디코드해 응답 프레임을 되돌려주는 식으로 UART 왕복을 흉내냅니다.

---

## 3부. 파일별 해설

### 3-1. `input_uart_session.hpp/.cpp` — RESPONSE 모델 동기 세션

- `Transact(command, payload) → InputTransactResult` : 프레임 인코딩 → 전송 → 응답 대기(최대
  `UART_ACK_TIMEOUT_MS`) → sequence가 일치하는 `RESPONSE`/`OPERATION_RESULT`를 명령 완료로 인정.
  응답 없으면 같은 sequence로 최대 `UART_MAX_RETRY_COUNT`회 재전송(STM32가 중복 요청을 캐시로 dedup).
  → `kSuccess / kRejected / kTimeout / kNotOpen / kTransportError / ...`
- **자발적 프레임 처리**: 명령 대기 중 도착한 `SENSOR_STATUS`/`DEVICE_STATUS`/`EVENT`(또는 매칭 안 되는
  프레임)는 `SetSpontaneousFrameHandler` 콜백으로 즉시 전달 → 절대 유실 안 됨.
- `PollSpontaneous(timeout)` : 유휴 시 자발적 프레임만 읽어 콜백으로 전달.
- **robustness**: timeout 시 `uart_parser_reset()`으로 반쯤 받은 프레임을 버려 재시도 응답 오염을 방지.

### 3-2. `input_node.hpp/.cpp` — MQTT ↔ UART 통역

transact가 **동기식**이라 linetracer의 비동기 pending 상태머신 없이 단순합니다.
`HandleMqttCommand()`가 UART 왕복까지 마치고 COMMAND_RESPONSE를 바로 발행합니다.

**MQTT 명령 → UART 매핑:**

| MQTT `ControlCommand` | UART 명령 | 비고 |
|---|---|---|
| `kStart` | `SET_SPEED` 후 `CONVEYOR_START` | `params.speed`(1~100), 생략 시 기본값 25. MCU 재부팅 복구를 위해 START마다 속도를 재전송 |
| `kStop` | `CONVEYOR_STOP` | |
| `kStatusRequest` | `CONVEYOR_GET_STATUS` | 응답의 컨베이어 상태를 별도 status로도 발행 |
| `kInitialize` | `INPUT_CONTROL_RESET` | 소프트 리셋(제어 오류 초기화). 동기 응답. 비상정지 latch 걸려있으면 STM32가 `ERR-EMERGENCY-STOP`으로 거부 |
| `kRecovery` | `RESET_DEVICE` | 비상정지 latch 해제(SafetyTask 경유). EMERGENCY_STOP처럼 STM32가 비동기 EVENT/DEVICE_STATUS로만 응답 → **fire-and-forget(`ExecuteAsync`)** |
| `EmergencyStop` | `EMERGENCY_STOP` | 별도 payload 타입. 비동기 응답 → **fire-and-forget** |
| `kRestart` / `kDestinationSet` | — (미지원) | UART 안 보내고 즉시 `kRejected` 응답 |

**UART 자발적 프레임 → MQTT 보고 매핑:**

| UART 프레임 | MQTT 채널 / 타입 | 내용 |
|---|---|---|
| `SENSOR_STATUS` (모든 측정) | event / `SENSOR_STATUS` | `{sensorId, measurementStatus, distanceCm}`. `measurementStatus`는 계약상 `OK`/`FAULT`만 허용 — **측정이 믿을 만한지**만 담는다. 거리값이 매번 바뀌므로 **측정마다 발행** |
| `SENSOR_STATUS` (FAULT로 전환 시) | error / `ERROR_OCCURRED` | 위 telemetry에 더해 `error_code=ERR-SENSOR` 알림도 발행. `current_state="SENSOR_{id}_FAULT"`로 sensorId를 실어 보냄(투입 쪽은 센서가 1개뿐이라 항상 id=1) |

> 센서 값은 telemetry라 `DEVICE_STATUS`가 아니라 **`SENSOR_STATUS` 이벤트**로 나간다(`device/{id}/event`,
> `mqtt_validation.hpp`가 SENSOR_STATUS를 device event로 분류). 덕분에 센서 활동이 장치 운영 상태
> (`current_state` = READY/EMERGENCY_STOP/RUNNING/STOPPED)를 덮어쓰지 않는다.

> **상자가 로봇팔 앞에 있는지는 이 노드도, STM32도 판단하지 않는다.** 거리값만 올리고
> 중앙 서버가 `server.ini`의 `[sensor_detection]` 임계값으로 판정해, 결과를
> `detectionStatus`(`DETECTED`/`CLEAR`/`UNKNOWN`) 필드로 붙여 Qt에 전달한다. 임계값
> 튜닝에 펌웨어 재플래시가 필요 없게 하려는 구조다. 이 노드가 보내는 메시지에는
> `detectionStatus`가 아예 없다.
| `DEVICE_STATUS` | status / `DEVICE_STATUS` | 장치 상태명 + 오류코드 |
| `EVENT` (heartbeat, id=1) | status / `DEVICE_STATUS` | 9바이트 payload 디코딩: `device_state`/`error_code`/uptime/투입·분류 센서 상태. 상태 변화 시에만 보고(uptime만 바뀌면 무시) |
| `EVENT` (safety, id=3, kind=1/3) | error / `ERROR_OCCURRED` | `ERR-SAFETY-ESTOP-LATCHED`(비상정지 latch) / `ERR-SAFETY-RESET-REJECTED`(해제 거부) |
| `EVENT` (safety, id=3, kind=2) | status / `DEVICE_STATUS` | 비상정지 해제 **성공**이라 에러가 아닌 상태로 발행(`current_state=READY`). STM32도 같은 시점에 `DEVICE_READY`로 전환함 |
| `EVENT` (health, id=4) | error / `ERROR_OCCURRED` | kind 디코딩: `ERR-HEALTH-UART-CHANNEL-TIMEOUT` / `ERR-HEALTH-QUEUE-OVERFLOW` / `ERR-HEALTH-SENSOR-STALE`(kind=3일 때 payload[3]의 sensorId를 `message`에 `sensorId=N`으로 포함) |

> EVENT의 `(id, kind, cause, sensorId)`가 바뀔 때만 보고(중복 재발 억제). sensorId는 HEALTH/SENSOR_STALE에서만 의미 있고(그 외 kind와 SAFETY 이벤트는 항상 dedup 시그니처에 NONE=0xFF로 들어감), 이걸 시그니처에 포함시킨 이유는 분류 쪽처럼 센서 여러 개가 같은 채널(cause)을 공유할 때 "다른 센서의 새 stale"이 "같은 센서의 재발"로 오인되어 억제되는 걸 막기 위함. async EVENT는 `current_state`를 덮지 않고 `error_code`만 갱신(heartbeat의 운영 상태 보존). 이벤트 ID/layout/validator는 Pi와 STM32가 `conveyor_events.h`를 함께 사용합니다. HEALTH EVENT payload는 8바이트(`[0]event_id [1]kind [2]cause [3]sensorId [4..7]timestamp`)입니다.

`UART` 오류코드 → MQTT `error_code` 매핑, `InputTransactStatus` → `CommandResult` 매핑도 여기서 담당.

### 3-3. `main.cpp` — 데몬 루프

`#ifdef LOGISTICS_INPUT_DAEMON_ENABLED`로 감싸고(아니면 스캐폴드 폴백), 단일 스레드 폴링 루프:

1. UART 닫혀 있으면 2초 주기 재연결 시도 (성공 시 `uart_connected=true` + status 발행)
2. 큐에 쌓인 MQTT 명령을 `InputNode.HandleMqttCommand()`로 처리 (각 명령이 UART 왕복 + 응답 발행)
3. `PollSpontaneous()`로 센서/상태 프레임 수신
4. UART 끊김 감지 시 세션 닫고 `UART_DISCONNECTED` status 발행 + 재연결 예약
5. 발행 대기 큐(outbox) flush (연결 상태일 때만)
6. 5초 주기 heartbeat
7. `SIGINT`/`SIGTERM` → graceful stop (세션 close → mosquitto stop)

MQTT 명령 콜백은 mosquitto 네트워크 스레드에서 실행되어 mutex 보호 큐(`CommandInbox`)에 넣고, 나머지는
전부 메인 스레드에서 처리합니다.

---

## 4부. 빌드·테스트

### 4-1. CMake 구조

`device-rpi/CMakeLists.txt` 상단에 **크로스플랫폼 라이브러리·테스트**(uart_transport, input_node,
input_uart_session)를 두고, 그 다음 `if(NOT LOGISTICS_BUILD_DEVICE_NODES) return()`으로 device-node
런타임(mosquitto/CURL/OpenCV)을 건너뜁니다. 루트 CMakeLists에 `LOGISTICS_BUILD_DEVICE_UART_TRANSPORT`
옵션(기본 ON)을 추가해, device 노드 전체를 끄고도 이 크로스플랫폼 부분만 configure/build/test할 수 있습니다.

### 4-2. 단위 테스트 (하드웨어 불필요)

- `device_uart_transport_test` — `UartTransport` read/write/timeout/재연결
- `device_input_uart_session_test` — transact 성공/거부/timeout/재시도/자발적 프레임/전송오류
- `device_input_node_test` — MQTT 명령 매핑·응답, 센서/장치 상태 보고, dedup, 미지원 명령

가짜 UART 백엔드(`tests/fake_input_uart_backend.hpp`)가 요청 프레임을 디코드해 응답을 되돌려주므로
실기기 없이 종단 로직을 검증합니다.

```sh
cmake -S . -B build -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
      -DLOGISTICS_BUILD_DEVICE_NODES=OFF -DLOGISTICS_BUILD_DEVICE_UART_TRANSPORT=ON
cmake --build build
ctest --test-dir build
```
> Windows에서는 `logistics::contracts`가 nlohmann_json을 요구하므로 vcpkg 매니페스트(`vcpkg.json`) 설치가
> 필요합니다. Pi에서는 `nlohmann-json3-dev`.

### 4-3. 배포 (`deploy/scripts/setup-input-node.sh`)

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_DEVICE_ID='PI-INPUT-01'
export LOGISTICS_UART_DEVICE='/dev/vedauart'
./deploy/scripts/setup-input-node.sh
```

`runtime/input-node/input-node.ini`를 생성하고 `logistics_input_node` 타깃을 빌드합니다. 실행은
`LOGISTICS_UART_DEVICE=... logistics_input_node <config>` (UART 경로는 두 번째 인자 또는 환경변수).

### 4-4. 실기기 종단 검증 시나리오 (아직 미완)

1. `setup-central-server.sh` + `setup-input-node.sh`로 양쪽 기동
2. Qt control-center 또는 `mosquitto_pub`으로 START → 실제 컨베이어 구동 + `device/{id}/response`에 SUCCESS
3. 박스로 초음파 센서 트리거 → `device/{id}/status`(또는 event)에 상태
4. UART 케이블 순간 분리/재연결 → `uart_connected=false→true`, 재연결 로그
5. mosquitto 재시작 → MQTT 재연결/LWT(오프라인 retain) 확인

---

## 부록 A. 알려진 한계 / 후속 조율 사항

1. **linetracer 브랜치와 공유 파일 겹침**: `uart_transport.*`, `mqtt_node_client`의
   `PublishResponse/PublishStatus`, `device-rpi/CMakeLists` 상단, 루트 CMakeLists 옵션은
   `feature/linetracer-node`와 내용이 겹칩니다(의도적으로 **동일하게** 작성해 병합 충돌 최소화).
   두 PR 중 먼저 merge되는 쪽 기준으로 한 번은 사람이 정리해야 합니다 — PR에 명시할 것.
2. **OpenCV 커플링**: 현재 device-node 빌드가 vision-node의 `find_package(OpenCV REQUIRED)`를 무조건
   호출해서, 카메라 없는 투입 Pi도 OpenCV 4.10.0이 필요합니다. 노드별 빌드 분리는 vision 담당 파일을
   건드려야 하므로 별도 과제로 남겨둡니다.
3. **`kRestart`/`kDestinationSet` 미지원**: 투입 역할과 무관해 UART로 보내지 않고 즉시 거부 응답합니다.
