# Gripper Raspberry Pi Node

그리퍼 노드는 중앙 서버의 이송 명령을 MQTT로 받아 STM32 그리퍼 컨트롤러의 UART 명령으로 변환하고, 로봇팔의
동작 진행 상황과 작업 결과를 다시 MQTT로 발행한다.

Vision 노드는 영상 처리 전용이며 이 노드를 대신하지 않는다. 상품 회전이나 6면 바코드 탐색 명령은 처리하지
않는다.

## 실행

최초 실행 전 예제 설정을 복사하고, 파일 내 주석에 따라 장치 IP·MQTT 정보와 **티칭 포즈**를 수정한다.

```bash
cp device-rpi/config/gripper-node.ini.example device-rpi/config/gripper-node.ini
nano device-rpi/config/gripper-node.ini
```

```bash
./logistics_gripper_node [gripper-node.ini] [/dev/vedauart]
```

인자를 생략하면 `device-rpi/config/node.ini`와 `/dev/vedauart`를 사용한다. 환경 변수로도 지정할 수 있다.

- `LOGISTICS_DEVICE_CONFIG`: 노드 설정 파일
- `LOGISTICS_UART_DEVICE`: UART 장치 노드

## 왜 포즈를 Pi가 갖고 있는가

STM32는 **계산된 관절 각도만** 받는다. 영상 좌표, 상자 치수, 카메라 캘리브레이션, 역기구학은 전부 Raspberry
Pi 몫이라고 UART 계약에 명시돼 있다.

그런데 현재 중앙 서버가 보내는 그리퍼 명령에는 좌표가 없고 `workId`와 `destination`만 들어있다. 그래서 이
노드는 **티칭한 고정 포즈**를 INI에서 읽어 사용한다. 서버가 나중에 `params.offsetX`로 픽셀 오프셋을 실어주면
`base_deci_deg_per_pixel` 값에 따라 집기 포즈의 base 관절만 보정한다. 기본값 `0.0`은 보정을 완전히 끄므로,
캘리브레이션 전에는 영상 값이 팔을 움직이지 못한다.

## MQTT → UART 명령 계약

| MQTT 명령 | `componentId` | STM32 UART 명령 | 비고 |
|---|---|---|---|
| `START`, `RESTART` | 생략/`gripper`/`arm` | 전체 사이클 (아래 참고) | `params.workId` 필수 |
| `START` | `pick` | 집기까지만 | 상자를 문 상태로 종료 |
| `START` | `transfer` | 이송 구간만 | |
| `START` | `place` | 놓기 구간만 | |
| `START` | `home` | `GRIPPER_HOME (0x22)` | `workId` 불필요 |
| `STOP` | — | `GRIPPER_STOP (0x23)` | 진행 중 사이클 취소 |
| `INITIALIZE` | — | `GRIPPER_RESET (0x25)` 성공 후 `GRIPPER_HOME (0x22)` | |
| `RECOVERY` | `safety` | `RESET_DEVICE (0x03)` | 1회 송신, Safety 이벤트로 결과 확인 |
| `RECOVERY` | 생략/`home` | `GRIPPER_HOME (0x22)` | 안전 확인 후 명시적 원점 복귀 |
| `STATUS_REQUEST` | — | `GRIPPER_GET_STATUS (0x24)` | homed 여부 재동기화 |
| `EMERGENCY_STOP` | — | `EMERGENCY_STOP (0xF0)` | 별도 messageType, 1회 송신 |

`EMERGENCY_STOP`은 `CONTROL_COMMAND`가 아니라 최상위 `messageType`이 `EMERGENCY_STOP`인 별도 메시지다.
`ControlCommandPayload::IsValid()`가 `command=EMERGENCY_STOP`을 거부하므로 그쪽으로 보내면 처리되지 않는다.

## 전체 사이클 순서

`START` 하나가 아래 10개 모션을 순서대로 실행한다. 각 모션은 **비동기**다. 명령의 RESPONSE는 "접수"만
의미하고, 실제 완료는 나중에 `MOTION_COMPLETE` 이벤트로 도착한다. 노드는 그 이벤트를 받은 뒤에만 다음
단계로 넘어간다.

```text
OPEN_CLAW      SET_GRIPPER(open)          → PREPARING
PICK_APPROACH  MOVE_ARM(pick_approach)    ┐
PICK_DESCEND   MOVE_ARM(pick)             ├ PICKING
CLOSE_CLAW     SET_GRIPPER(closed)        ┘
PICK_RETREAT   MOVE_ARM(pick_approach)    ┐ TRANSFERRING
PLACE_APPROACH MOVE_ARM(place_approach)   ┘
PLACE_DESCEND  MOVE_ARM(place)            ┐
RELEASE_CLAW   SET_GRIPPER(open)          ├ PLACING
PLACE_RETREAT  MOVE_ARM(place_approach)   ┘
RETURN_HOME    HOME                        → HOMING → COMPLETED
```

서보에 위치 피드백이 없어서 컨트롤러는 보간 시간이 지나면 완료로 간주한다. 노드는 각 모션에 `보간시간 + 2초`
예산을 주고, 그 안에 완료 이벤트가 없으면 `ERR-GRIPPER-MOTION-TIMEOUT`으로 사이클을 중단한다.

## UART → MQTT 보고 계약

| STM32 프레임 | MQTT 발행 |
|---|---|
| `RESPONSE` (명령 접수/거부) | `device/{id}/response`의 `COMMAND_RESPONSE` |
| `EVENT` `MOTION_COMPLETE` | 다음 단계 진행, 단계 변경 시 `DEVICE_STATUS` |
| `EVENT` `FAULT` | `device/{id}/error`의 `ERROR_OCCURRED` + 사이클 중단 |
| `EVENT` `SAFETY` | E-Stop 래치/해제 상태 발행 |
| `EVENT` `HEARTBEAT` | 상태 변화 시에만 `DEVICE_STATUS` |
| `DEVICE_STATUS` | `DEVICE_STATUS` |

### 이벤트 ID가 겹치는 함정

그리퍼 펌웨어는 두 계열이 같은 event ID를 쓴다. **길이로 구분해야 한다.**

| event_id | length | 의미 |
|---|---|---|
| `0x01` | 7 | `APP_EVENT_HEARTBEAT` |
| `0x01` | 4 | `GRIPPER_EVENT_MOTION_COMPLETE` |
| `0x02` | 3 | `APP_EVENT_SAFETY` |
| `0x02` | 5 | `GRIPPER_EVENT_FAULT` |

길이를 확인하지 않으면 heartbeat를 모션 완료로 오독해서 사이클이 한 단계씩 앞서 나간다.

## 순서·중복·상태 정책

1. 한 번에 하나의 사이클만 활성화된다. 같은 `workId`가 다시 오면 모터를 다시 돌리지 않고 `DUPLICATED`로
   응답하고, 다른 `workId`가 오면 `ERR-ACTIVE-CYCLE-CONFLICT`로 거부한다. 이미 상자를 문 상태에서 다른 작업을
   시작하면 들고 있던 상자를 떨어뜨리기 때문이다.
2. 같은 `requestId`가 재전송되면 저장된 응답을 다시 발행하고 UART 동작은 반복하지 않는다.
3. MQTT 콜백은 명령을 최대 64개 큐에 넣는다. 포화 시 거부 로그를 남긴다.
4. **작업 중에는 `READY`/`COMPLETED`/`PLACED` 상태를 발행하지 않는다.** 중앙 서버 오케스트레이터는 `jobId`가
   붙은 `DEVICE_STATUS`의 `currentState`가 이 셋 중 하나면 그리퍼 작업이 끝난 것으로 판정한다. 그래서
   진행 중에는 `PICKING`/`TRANSFERRING`/`PLACING`만 쓰고, `COMPLETED`는 사이클이 실제로 끝날 때 정확히 한 번만
   발행한다. 사이클 진행 중에는 컨트롤러 heartbeat의 `READY`도 덮어쓰지 않고 무시한다.
5. heartbeat에는 그리퍼 전용 `device_id`와 현재 상태가 담긴다. 상태가 바뀔 때만 갱신하므로 1Hz heartbeat가
   상태 토픽을 채우지 않는다.

## 안전 동작

이 설계에는 서보 전원을 물리적으로 끊는 릴레이가 없다. E-Stop 시 컨트롤러는 보간을 멈추고 활성 모션을
무효화한 뒤 **마지막 PWM 값을 유지**한다. 들고 있던 상자를 즉시 떨어뜨리지 않기 위한 선택이며, 독립적인
안전 등급 차단 수단은 아니다.

E-Stop이 걸리면 컨트롤러의 `homed` 기준이 사라진다. 그래서 이 노드도 다음을 지킨다.

- E-Stop 이벤트를 받으면 진행 중 사이클을 중단하고 내부 `homed`를 내린다.
- `RESET_DEVICE`는 래치만 풀고 **자동으로 원점 복귀하지 않는다.** 해제 직후 상태는 `READY`가 아니라
  `STOPPED`로 발행한다. `READY`로 보고하면 서버가 바로 작업을 내려보내는데 컨트롤러는 그걸 거부하기 때문이다.
- 작업 구역이 안전한지 확인한 뒤 `RECOVERY`(component `home`) 또는 `INITIALIZE`로 **명시적으로** 원점 복귀를
  지시해야 다시 작업을 받을 수 있다.
- homed가 아닌 상태에서 들어온 사이클 요청은 UART로 보내기 전에 `ERR-GRIPPER-NOT-HOMED`로 거부한다.
- UART 재연결 직후에도 팔의 자세를 알 수 없으므로 `homed`를 내린다. `STATUS_REQUEST`로 컨트롤러의 실제
  `homed` 값을 다시 읽어올 수 있다.

## 현재 검증 범위

단위 테스트(`device_gripper_node_test`)는 가짜 UART backend로 명령 변환, 10단계 사이클 진행, 중복·충돌 처리,
모션 fault, 지연된 완료 이벤트 무시, E-Stop 복구 순서, 모션 타임아웃, 포즈 보정과 INI 파싱을 검증한다.

실제 배선 후에는 티칭 포즈를 실기기에 맞춰 조정하고, `/dev/vedauart`와 MQTT broker를 함께 사용해 전체 사이클과
E-Stop 복구를 확인해야 한다.
