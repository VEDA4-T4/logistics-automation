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

경로는 두 가지고, `params.targetPose`가 있는지로 갈린다.

| 입력 | 처리 |
|---|---|
| `params.targetPose` 있음 | 역기구학으로 관절 각도 계산 |
| 없음 | INI에 **티칭한 고정 포즈** 사용 (`params.offsetX`가 있으면 base 관절만 픽셀 보정) |

링크 길이는 실기기를 자로 측정한 값(2026-07-30)이다: shoulder-elbow 85mm, elbow-클로 파지중심 175mm(전완
110mm + 클로 65mm, 손목 관절이 없어서 강체로 이어짐). 손목 회전축이 없다는 게 핵심인데, 그래서 elbow_to_tcp가
하나의 링크로 합쳐진다. shoulder-offset(베이스축에서 shoulder 피벗까지의 수평 거리)은 아직 측정하지 못해 0으로
가정했다.

### targetPose 계약

중앙서버 `ProcessOrchestrator::MakeGripperCommand()`(homography 활성화 시, `central-server-rpi/src/process_manager/process_orchestrator.cpp`)가 실제로 보내는 형태 그대로다.

```json
{
  "requestId": "...",
  "command": "START",
  "targetDeviceId": "PI-GRIPPER-01",
  "componentId": "gripper",
  "params": {
    "workId": "3f2504e0-4f89-11d3-9a0c-0305e82c3301",
    "destination": "1",
    "action": "PICK",
    "coordinateFrame": "PI-GRIPPER-01_BASE",
    "unit": "mm",
    "targetPose": { "x": 220.0, "y": 0.0, "z": 20.0, "rollDeg": 180.0, "pitchDeg": 0.0, "yawDeg": 37.5 },
    "box": { "length": 200.0, "width": 150.0, "height": 90.0 },
    "calibrationVersion": 1
  }
}
```

이 노드가 읽는 건 **`targetPose.x/y/z`뿐**이다. `action`, `coordinateFrame`, `unit`, `box`, `calibrationVersion`,
그리고 `targetPose.rollDeg`/`pitchDeg`/`yawDeg`는 전부 무시한다. `unit`은 항상 `"mm"`으로 고정 발행되고
`coordinateFrame`은 항상 이 노드 자신의 기준(`PI-GRIPPER-01_BASE`)이라, 지금은 검증할 다른 값이 없다 —
그리퍼가 여러 대가 되어 좌표계가 갈리면 그때 검사를 추가한다.

좌표계는 **밑판의 베이스 회전축이 원점**, `+Z`는 위, `+X`는 base 서보가 `base_zero_deci_deg`일 때 팔이 향하는
방향, 단위는 mm다. 서버의 homography 변환(`HomographyTransformer::Transform()`)이 픽셀 좌표를 이미 이
기준으로 회전·평행이동해서 내보내므로, 노드 쪽에서 추가 변환은 없다.

**상자 방향(`yawDeg`)은 받아도 쓰지 않는다.** 팔이 base·shoulder·elbow 3자유도뿐이고 손목 회전 관절이 없어서,
클로의 파지축이 base 각에 고정으로 종속된다. 그런데 base 각은 목표의 `(x, y)`가 이미 결정한다. 즉 위치와 파지
방향을 독립적으로 정할 자유도가 애초에 없다. 근사해서 쓰는 대신 아예 무시하는 쪽을 택한 건, 못 맞추는 걸 맞춘
척하면 파지 실패 원인이 보이지 않기 때문이다. `rollDeg`/`pitchDeg`도 이 팔에 대응하는 관절이 없어 마찬가지로
무시한다.

### 검증에서 거부되는 것들

`targetPose`가 있으면 **집기 자세와 그 위 접근 자세를 둘 다 먼저 풀고**, 하나라도 실패하면 사이클을 시작하지
않는다. 접근만 풀고 출발하면 상자 위까지 올라간 다음에 하강이 불가능하다는 걸 알게 되고, 그 시점의 중단은 팔이
컨베이어 위에 걸린 상태로 끝난다.

| 오류 코드 | 원인 |
|---|---|
| `ERR-GRIPPER-POSE-MALFORMED` | `x`/`y`/`z` 누락, 숫자 아님, 비유한값, 또는 단위 착오(±10000mm 초과) |
| `ERR-GRIPPER-POSE-BELOW-PLATE` | `z`가 `min_target_z_mm` 미만 |
| `ERR-GRIPPER-UNREACHABLE-FAR` | 두 링크 합(기본 260mm)보다 멀다 |
| `ERR-GRIPPER-UNREACHABLE-NEAR` | 링크 차(기본 90mm)보다 가깝다 — 접을 수 없는 사각지대 |
| `ERR-GRIPPER-JOINT-LIMIT` | 도달은 가능하나 특정 관절이 펌웨어 한계를 벗어남 (메시지에 관절명 포함) |

기구 형상에서 바로 나오는 제약 두 개는 서버 배치 설계에 영향이 있다.

- **base 회전은 ±80°뿐이다** (`base_min/max_deci_deg` 100~1700). 팔의 정면에서 90° 옆에 있는 컨베이어는
  거리와 무관하게 서비스할 수 없다.
- **전완쪽 링크(175mm)가 상완(85mm)보다 훨씬 길다.** 그래서 가깝고 높은 점은 상완을 수직 뒤로 넘겨야 닿는데,
  shoulder 기준이 90.0°라 그건 표현 자체가 안 된다. 밑판 높이에서 집으려면 팔이 거의 완전히 펴져야 하므로,
  **컨베이어가 밑판보다 낮게 놓이는 배치가 정상**이고 그 경우 `min_target_z_mm`을 음수로 설정해야 한다.

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

`targetPose`로 시작한 사이클은 `PICK_APPROACH`/`PICK_DESCEND`/`PICK_RETREAT`가 계산된 자세를 쓴다. 놓기 구간
(`place_approach`, `place`)은 서버가 좌표를 주지 않으므로 여전히 교시 포즈다.

## duration을 노드가 계산해야 하는 이유

STM32는 **너무 짧은 duration을 거부하지 않고 조용히 늘린다.** `gripper_control.c`가 실제로 쓰는 값은
`max(요청값, 자기 속도한계상 최소값)`이고, 늘렸다는 사실은 알려주지 않는다.

그래서 노드가 같은 식(`gripper_control_arm_minimum_duration()`)을 그대로 복제해서 갖고 있다. 안 그러면 이런
일이 난다. shoulder 최대속도는 120 deci-deg/s니까 600 deci-deg 이동은 실제로 5초가 걸리는데, 노드가 명목값
1500ms만 믿고 `1500 + 2000ms`에 타임아웃을 잡으면 **정상 진행 중인 모션을 완료 이벤트 유실로 오판하고 중단**한다.

| 상황 | 예산 |
|---|---|
| 현재 자세를 아는 경우 | `max(명목 duration, 속도한계상 최소) + 2초` |
| 모르는 경우 | 관절 한계상 최악값 10초 + 2초 |
| `HOME` | 항상 10초 + 2초 (컨트롤러가 팔·클로 양쪽으로 늘리고, 클로 이동량을 노드가 모를 수 있음) |

"현재 자세를 모르는 경우"는 **보간 도중에 멈춘 상태**다. FAULT, E-Stop, 운전자 `STOP`, 모션 타임아웃 — 어느
쪽이든 팔은 양쪽 다 이름 붙일 수 없는 중간 자세에 있다. 이때 마지막으로 지시한 목표를 현재 위치로 착각하면
다음 이동 시간을 과소평가해서 또 타임아웃을 낸다. 그래서 추적을 포기하고, `HOME` 완료나 `GET_STATUS` 응답이
실제 각도를 알려줄 때까지 최악값을 쓴다.

`GET_STATUS`는 팔이 정지 상태(`IDLE`/`STOPPED`)일 때만 각도를 반영한다. 보간 중의 각도는 이미 지나간 자세다.

## 펌웨어 상수를 복제해서 갖고 있는 부분

`[gripper]` 섹션의 관절 한계·속도 한계는 `stm32/gripper-controller/Application/Inc/gripper_calibration.h`의
사본이다. 중복이지만 의도적이고, 이유가 두 개다.

- **한계**: UART 계약은 0~180.0° 전체를 허용하지만 펌웨어는 더 좁은 범위만 받고 벗어나면 `INVALID_PAYLOAD`만
  돌려준다. 이쪽에서 먼저 검사하면 어느 관절이 문제인지 이름을 붙여서 서버에 올릴 수 있다.
- **속도**: 위의 duration 계산에 필요하다.

**펌웨어 헤더를 튜닝하면 INI도 같이 고쳐야 한다.** 한쪽만 바꾸면 조용히 어긋난다. 계약 헤더
(`shared/include/logistics/contracts/uart/gripper_commands.h`)로 올려서 단일 출처로 만드는 게 진짜 해결이지만,
그건 STM32 담당과 합의가 필요하다.

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
- UART 재연결 직후에도 팔의 자세를 알 수 없으므로 `homed`를 내리고, duration 계산용 각도 추적도 함께 포기한다.
  `STATUS_REQUEST`로 컨트롤러의 실제 `homed` 값과 각도를 다시 읽어올 수 있다.

## 현재 검증 범위

`device_gripper_node_test`는 가짜 UART backend로 명령 변환, 10단계 사이클 진행, 중복·충돌 처리, 모션 fault,
지연된 완료 이벤트 무시, E-Stop 복구 순서, 모션 타임아웃, 포즈 보정과 INI 파싱을 검증한다. 여기에 더해
`targetPose` 경로(계산된 각도가 실제로 요청 좌표로 정기구학 역산되는지), 도달 불가·형식 오류·밑판 아래 목표의
거부, 좌표 없는 명령의 교시 경로 유지, 그리고 속도한계 기반 duration과 추적 상실 시 최악값 예산을 다룬다.
서버의 실제 페이로드 형태(`rollDeg`/`pitchDeg`/`yawDeg`/`box`/`coordinateFrame`/`unit`/`calibrationVersion`을
`targetPose`와 함께 보내는 것)를 그대로 재현한 회귀 테스트도 포함한다 — 계약 키 이름이 어긋나는 걸 이런
테스트 없이는 못 잡는다(`pickPose`로 잘못 가정했던 최초 구현이 그 사례).

`device_gripper_kinematics_test`는 역기구학을 따로 검증한다. 손으로 계산 가능한 정삼각형·완전신장 케이스, 도달
경계에서 부동소수 잡음으로 실패하지 않는지, 관절 한계가 어느 관절 때문인지 이름을 붙이는지, 정기구학 왕복,
그리고 펌웨어 duration 공식과 자릿수까지 일치하는지를 확인한다.

**실기기 검증은 아직 안 됐다.** 링크 길이(85/175mm)와 shoulder 높이(20mm)는 조립된 팔을 자로 재서 넣었지만,
`shoulder_offset_mm`은 아직 미측정(0으로 가정)이고 나머지도 확인이 필요하다. 배선 후 순서는 이렇다.

1. `shoulder_offset_mm`(베이스 회전축에서 shoulder 피벗까지 수평 거리) 실측. 0이 아니면 베이스축 근처 목표의
   yaw가 눈에 띄게 틀어진다.
2. 관절 방향(`*_direction`)과 기준각(`*_zero_deci_deg`)을 실기기로 확인한다. 부호가 틀리면 팔이 반대로 간다.
3. 컨베이어와 밑판의 상대 높이를 재서 `min_target_z_mm`을 정한다(음수일 가능성이 높다 — 전완쪽 링크가
   상완보다 길어서 밑판 높이에서 집으려면 팔이 거의 완전히 펴져야 한다).
4. `STATUS_REQUEST`로 읽은 각도와 `targetPose`로 지시한 좌표를 정기구학으로 대조해 캘리브레이션을 마무리한다.
