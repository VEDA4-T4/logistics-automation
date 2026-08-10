#import "template2.typ": *

#show: project.with(title: "시스템 및 상세 설계서 (SDD)", authors: authors)

= 설계 기준 및 상세 설계

본 문서는 Pull Request \#115의 통합 결과(이하 기준 구현)를 설계 기준선으로 삼는다. 이전 문서의 장과 공정 흐름은 유지하되, 현재 코드에서 확인되지 않는 4채널 영상, 상자 6면 회전, 범용 AI 객체 분류 기능은 설계 범위에서 제외한다.

== 문서 개요

=== 목적

스마트 물류 자동화 시스템의 구성요소, 책임 경계, 상태 전이, 통신 계약, 데이터 저장, 오류·복구 및 배포 구조를 구현과 추적 가능한 수준으로 정의한다. 이 문서는 제품 요구사항 정의서(PRD)와 시스템 요구사항 명세서(SRS)를 코드 구조 및 시험 항목으로 연결하는 기준이다.

=== 적용 범위

- Windows 관제 PC의 Qt 6 Control Center
- 중앙 서버 Raspberry Pi의 C++ 서비스, SQLite, MQTT 및 HTTP 업로드
- Input, Vision, Gripper, Sorting, Line Tracer 장치 노드
- 장치 Raspberry Pi와 STM32 사이의 공용 UART 프레임 및 역할별 명령
- 단일 CCTV RTSP 영상과 선택적 ONVIF 메타데이터 오버레이
- 배포 설정, 보존 정책, 오류·비상 정지·복구 흐름

실제 배선, 센서 임계값 보정, 카메라 설치 위치, 그리퍼 서보 각도, 컨베이어 속도, 게이트 각도와 처리량은 설치 환경별 실기 검증 항목이다.

=== 설계 기준과 용어

#table(
  columns: (1.1fr, 2.7fr),
  inset: 4pt,
  [*용어*], [*정의*],
  [Work], [중앙 서버가 `BOX_DETECTED`를 수락할 때 UUID로 생성하는 물류 작업 단위],
  [Device Node], [MQTT와 장치 제어를 연결하는 Raspberry Pi 프로세스],
  [A/B/C], [연속 좌표가 아닌 출발·도착 노드의 이산 위치 식별자],
  [Confirmed Position], [Line Tracer가 실제로 확인한 현재 노드. 출발/도착 영역과 A/B/C 위치로 표현],
  [Movement State], [`IDLE`, `MOVING`, `ARRIVED`로 구성된 UI 위치 계약],
  [Safety Latch], [위험이 해소되고 승인된 reset 또는 recovery가 수행될 때까지 구동을 막는 장치 상태],
  [기준 구현], [Pull Request \#115의 통합 브랜치를 문서 브랜치에 병합한 코드],
)

== 시스템 개요

=== 설계 목표

시스템은 단일 상자를 감지하고 작업 ID를 부여한 뒤, 박스 ROI와 바코드를 인식하여 목적지를 결정하고, Gripper·Sorting·Line Tracer를 순서대로 제어한다. 관제자는 단일 CCTV 영상, 제품 정보, 장치 상태, 탑뷰 공정 상태, 작업 목록과 운영 로그를 확인하고 시스템 제어 명령을 전송한다.

자동 공정 오케스트레이션은 중앙 서버 설정의 feature flag가 켜지고 필수 장치 ID가 올바르게 매핑된 경우에만 동작한다. Homography 좌표 변환도 설치 카메라에서 측정한 행렬과 calibration version이 준비된 경우에만 활성화한다.

=== 정상 공정 시나리오

#enum(
  [Input 장치가 상자를 감지하고 `BOX_DETECTED` 이벤트를 발행한다.],
  [중앙 서버가 이벤트를 영속화하고 UUID `workId`를 생성하여 `WORK_CREATED`를 Vision과 Qt에 전달한다.],
  [Vision이 카메라 프레임에서 스티로폼 박스 ROI를 검출하고 위치·모서리 및 바코드 결과와 선택적 이미지를 발행한다.],
  [중앙 서버가 상품 카탈로그를 조회한다. 등록 상품은 목적지를 사용하고, 미등록 상품은 설정된 기본 목적지가 있을 때만 계속 처리한다.],
  [Homography가 활성화된 경우 픽셀 모서리를 컨베이어·그리퍼 좌표로 변환해 Gripper 명령에 포함한다.],
  [Gripper가 HOME을 전제로 PICK/이송 시퀀스를 수행하고 motion ID가 일치하는 완료 이벤트를 단계별로 확인한다.],
  [Sorting이 하나의 활성 cycle에 목적지 1~3을 설정하고 컨베이어·게이트를 제어한다.],
  [Line Tracer가 현재 위치와 A/B/C 목적지를 바탕으로 운송한다.],
  [하역 완료 이벤트가 중앙 서버의 `WORK_COMPLETED`로 연결되고 Qt 활성 작업 목록에서 제외된다.],
)

Line Tracer의 교차점은 업무 위치로 송수신하지 않는다. 관제 탑뷰에서도 교차점 정지를 표현하지 않으며 Line Tracer는 확인된 출발 또는 도착 노드에 놓이거나, `FOLLOWING_LINE` 중 두 노드 사이를 이동하는 것으로 표현한다.

== 전체 아키텍처

=== 구성요소 관계

#block(fill: rgb("f4f6f8"), inset: 8pt, breakable: false)[
```text
PNO-A9081RG CCTV -- RTSP/TCP + ONVIF --> Qt Control Center
                                             |
                                             | MQTT/TLS, HTTP history/image
                                             v
Device RPi nodes <--- MQTT/TLS ---> Central Server ---> SQLite/files
      |
      | UART 115200, framed protocol
      v
    STM32 ---> motors, servos, line/FSR/ultrasonic sensors
```
]

배포 CCTV는 PNO-A9081RG 1대를 사용한다. 현재 코드는 모델명을 강제하지 않고 `channel_1_url`과 선택적 metadata URL만 사용하므로, 해당 장비의 프로파일·코덱·ONVIF 호환성은 설치 시험에서 확인한다.

=== 책임 분리

#table(
  columns: (1.1fr, 2.7fr),
  inset: 4pt,
  [*구성요소*], [*주요 책임과 경계*],
  [Shared Contracts], [MQTT/HTTP 데이터 형식과 검증, 역할별 장치 상태 의미, UART codec·CRC·parser. broker나 장치 동작 자체는 수행하지 않음],
  [Central Server], [장치 등록·heartbeat, 명령 집계, 작업 상태기계, 상품 조회, 영속화, HTTP 업로드·이력, 보존 작업],
  [Qt Control Center], [관제·제어·단일 RTSP 재생·ONVIF overlay. 장치의 물리 상태를 추정해 제어하지 않음],
  [Device RPi], [MQTT 명령을 역할별 UART로 변환하고 UART 상태·이벤트를 MQTT로 재발행],
  [STM32], [센서·모터·서보의 실시간 상태기계, 안전 latch와 장치 이벤트 생성],
)

=== 중앙 서버 내부 구조

- MQTT Core/Transport: topic 구독, envelope 검증, publish와 reconnect
- Device Manager: 장치 역할·연결·상태·현재 job·오류·Line Tracer 위치·heartbeat 관리
- Command Manager: `requestId`별 대상 응답, 중복·지연 응답, 실패와 timeout 집계
- MQTT Handler: 계약 검증, 영속화, 카탈로그 조회, Qt routing
- Process State Machine/Orchestrator: 공정 전이 preview·commit과 후속 명령
- Persistence/Retention: SQLite migration, 이벤트·작업·오류·보안·업로드 기록과 기간 만료 삭제
- HTTP Upload Server: 이미지·로그 업로드, 파일 조회와 과거 이력 API

DB 저장 성공과 후속 MQTT 발행·공정 전이는 하나의 분산 트랜잭션이 아니다. 설계는 이벤트를 먼저 저장하고 이후 단계 실패를 작업 실패 또는 운영 오류로 남기는 경계를 갖는다.

== 하드웨어 설계

=== 관제 및 영상

- 관제 PC: Qt 6.10 이상, MQTT·Multimedia·Network·Widgets 모듈
- CCTV: PNO-A9081RG 1대, RTSP H.264 프로파일 1개와 선택적 ONVIF metadata track
- 영상 경로: CCTV에서 관제 PC로 직접 RTSP/TCP 연결
- 보안 경계: RTSP 자체는 암호화하지 않으므로 신뢰 LAN 또는 VPN에서 사용한다.

=== Raspberry Pi와 STM32

#table(
  columns: (1.1fr, 1.3fr, 1.5fr),
  inset: 4pt,
  [*역할*], [*RPi 노드*], [*STM32/물리 기능*],
  [Input], [컨베이어 명령·속도·상태 중계], [투입 모터, 감지 센서, safety latch],
  [Vision], [libcamera/GStreamer, OpenCV 검출], [직접 STM32 제어 없음],
  [Gripper], [다단계 작업 sequencer], [arm·gripper servo, HOME, stop],
  [Sorting], [work↔cycle, route 1~3], [컨베이어, MG90S gate, 센서],
  [Line Tracer], [작업·현재 위치·UART 연결], [라인/marker, FSR, 초음파, 모터, safety],
)

Gripper servo는 위치 피드백이 없으며 시간 경과와 motion complete 이벤트로 단계 완료를 판단한다. 따라서 명령 완료와 실제 파지 성공은 서로 다른 검증 대상이다. 독립 안전 전원 차단을 구현한 것으로 간주하지 않는다.

== 소프트웨어 상세 설계

=== Qt Control Center

메인 화면은 명령 패널, 단일 영상 영역, 공장 탑뷰, 운영 대시보드, 활성 작업/상품 결과 목록, 운영 로그로 구성된다. 테스트용 목업 제어창은 현 구현과 배포 범위에 없다.

==== 단일 영상과 오버레이

- 채널 수는 코드에서 1로 고정되고 UI는 1 CHANNEL/1×1 레이아웃을 사용한다.
- `channel_1_url`로 RTSP/TCP H.264를 수신한다.
- low-latency, network timeout, probe size, 최대 입력 buffer를 설정한다.
- ONVIF XML metadata가 있으면 bounding box를 영상 좌표에 overlay하고, 기본 1500 ms 동안 새 metadata가 없으면 제거한다.
- RTSP 인증 실패, timeout, 비정상 응답, H.264 track 부재, buffer 한도 초과는 운영 로그와 재연결 흐름으로 연결한다.

==== 작업과 제품 표시

상단 작업 선택은 드롭다운이 아닌 리스트다. 선택 작업의 이미지, 인식 상태, 상품명·코드·목적지·신뢰도·상세 메시지를 표시한다. HTTP 상대 이미지 경로는 중앙 서버 URL로 해석하며 최대 10 MiB와 redirect 정책을 적용한다.

`WORK_COMPLETED`는 작업 성공/실패를 기록하고 활성 목록에서 숨긴다. `CurrentProductState` 메모리에서 즉시 삭제하지는 않으므로 장시간 실행 시 무한 증가 가능성은 운영 제약이며, 서버의 영속 이력 삭제 정책과 구분한다.

==== 탑뷰 상태와 애니메이션

- Sorting은 우측, 목적지 A/B/C는 좌측에 배치한다.
- Line Tracer는 `confirmedPosition`이 제공한 출발/도착 노드에 고정한다.
- 목적지가 정해진 `STOPPED` 상태에서는 경로와 흰색 방향 표식을 표시하되 움직이지 않는다.
- `FOLLOWING_LINE`에서만 방향 표식과 Line Tracer 이동 애니메이션을 진행한다.
- 도착·하역 완료 시 경로 표식을 숨기고 노드 위치를 갱신한다.
- Input/Sorting 컨베이어의 흐름 표시는 해당 역할의 working 상태에만 움직이며 OS의 reduced-motion 설정을 존중한다.
- `ESTOP`의 빨간색 표현은 recovery 성공 또는 `STOPPED`/`RECOVERY_READY` 상태가 확인된 뒤 중립 상태로 돌아간다.

Line Tracer 상태 snapshot은 `departurePosition`, `targetPosition`, `confirmedPosition`을 모두 요구한다. 각 위치는 `area: DEPARTURE|DESTINATION`과 `location: A|B|C`이며 movement state는 `IDLE|MOVING|ARRIVED`이다. 누락되거나 범위를 벗어난 snapshot 전체는 적용하지 않는다.

==== 제어와 로그

일반 명령은 UI에서 선택한 SYSTEM 또는 장치를 대상으로 하고, Emergency Stop은 항상 SYSTEM으로 보낸다. Start, Stop, Recovery, ESTOP의 pending 응답을 구분하며 ESTOP은 일반 명령이 pending이어도 사용할 수 있다. 실시간 MQTT 로그는 message ID로 중복 표시를 억제하고 HTTP cursor 기반 과거 이력을 추가 조회한다.

=== Vision 노드

==== 프레임 취득과 재연결

Vision 노드는 GStreamer `libcamerasrc` pipeline으로 프레임을 열고, 개방 실패 또는 프레임 손실 시 제한된 재연결 흐름을 수행한다. 중앙 서버가 발행한 `WORK_CREATED`를 기다리며 로컬에서 work ID를 생성하지 않는다.

==== 박스와 바코드 검출

#enum(
  [HSV 색상 범위로 스티로폼 후보 mask를 만든다.],
  [morphology와 contour를 적용하고 면적·종횡비·직사각형 조건을 통과한 가장 큰 ROI를 고른다.],
  [ROI와 네 모서리 또는 position 결과를 work ID와 함께 발행한다.],
  [OpenCV barcode detector로 영역을 찾고 값을 decode한다.],
  [연속 실패 임계값 이후에만 대비 향상, 원근 보정, Bicubic 또는 외부 FSRCNN 모델을 fallback으로 사용한다.],
)

이는 일반 객체 분류 모델이 아니며 한 면의 바코드를 대상으로 한다. 제품을 회전하여 6면을 촬영하는 설계는 현재 범위에 없다. 외부 FSRCNN 모델은 저장소에 포함되지 않는 배포 자산이다.

==== 영상 업로드 경계

Vision은 HTTP 업로드에 성공한 뒤 upload ID, work ID, device ID, 경로, checksum을 포함한 `PRODUCT_IMAGE` MQTT 참조를 발행한다. 중앙 서버는 DB의 업로드 기록과 모든 참조가 일치할 때만 이벤트를 수락한다. 업로드 실패 frame 보존은 별도 failure-frame 정책을 따른다.

=== Input 노드

Input RPi는 `START/RESTART`, `STOP`, 속도 설정, 상태 요청, reset, recovery, ESTOP을 UART 명령으로 바꾼다. START 전 필요한 속도를 먼저 전송하고 UART controller가 재연결되면 캐시한 속도를 재적용한다. STM32는 fault 또는 safety latch 중 구동 명령을 거부하며 상태·센서·이벤트를 RPi로 보낸다.

=== Gripper 노드

작업 시작에는 유효한 work ID와 HOME 상태가 필요하다. RPi sequencer는 arm 이동과 gripper 개폐를 단계별로 명령하고, 예상 motion ID와 일치하는 `MOTION_COMPLETE`만 다음 단계의 근거로 사용한다. timeout, stale completion, fault, ESTOP이면 cycle을 중단한다. Initialize는 reset 후 HOME, safety release 뒤 recovery는 재-HOME을 수행한다.

=== Sorting 노드

`DESTINATION_SET`은 목적지 A/B/C를 UART route 1/2/3과 cycle ID에 연결한다. UART ACK가 성공한 뒤에만 work↔cycle을 활성화한다. 같은 work 명령은 멱등 처리하고 다른 work가 진행 중이면 거부한다. 단일 활성 cycle을 사용하며 HOME, MOVING, WAIT_ITEM, RETURNING, FAULT gate 상태와 cycle complete를 보고한다.

=== Line Tracer 노드

==== 위치와 경로

A/B/C는 물리 좌표가 아니라 route ID 1/2/3이다. 초기화는 reset 후 현재 위치를 설정해야 하며 현재 위치가 미확정이면 목적지 명령을 STM32에 전달하지 않는다. STM32 route planner는 현재 destination에서 공용 line을 통해 pickup으로 이동하고 적재를 확인한 뒤 지정 destination으로 복귀한다. 교차점 위치 메시지는 없다.

==== 상태와 이벤트

- 상태: `IDLE`, `LOAD_WAIT`, `FOLLOWING_LINE`, `CORRECTING`, `ARRIVED`, `UNLOADING`, `STOPPED`, `FAULT`, `EMERGENCY_STOP`
- 이벤트: `STARTED`, `ARRIVED`, `LOAD_DETECTED`, `UNLOAD_COMPLETE`, `STATE_CHANGED`, `FAULT`, `HEARTBEAT`
- 센서: line debounce·marker one-shot, FSR 적재/과적 hysteresis, 초음파 장애물과 센서 staleness

Safety task는 ESTOP, line lost, obstacle, load lost, overload, turn/marker/communication timeout과 sensor fault를 latch한다. 정상 STOP은 조건부 resume이 가능하지만 safety latch는 승인된 reset 전까지 drive·resume·reset 요청을 제한한다. UART 재연결만으로 물리 위치나 진행 중 motion이 복원되지는 않으므로 status 조회, 현재 위치 재확인과 필요 시 재초기화를 수행한다.

== 통신 상세 설계

=== MQTT

모든 메시지는 protocol version `1.0`, `messageId`, `messageType`, `sourceId`, ISO-8601 timestamp, data를 포함한다. 현재 version 1.0 외 메시지는 호환 처리하지 않는다.

#table(
  columns: (1.6fr, 2.2fr),
  inset: 4pt,
  [*토픽 계열*], [*방향/용도*],
  [`server/request/{client}`], [Qt → 중앙 서버 제어 요청],
  [`qt/{client}/{response,status,event,error}`], [중앙 서버 → 특정 Qt 응답·상태·이벤트·오류],
  [`device/{id}/register`], [장치 → 중앙 서버 등록],
  [`device/{id}/{status,event,error,heartbeat}`], [장치 → 중앙 서버 telemetry],
  [`device/{id}/command`], [중앙 서버 → 장치 명령],
  [`device/{id}/response`], [장치 → 중앙 서버 명령 응답],
  [`system/broadcast/command`], [중앙 서버 → 전체 장치 시스템 명령],
)

topic endpoint와 source ID, command target, topic별 message type을 교차 검증한다. 잘못된 JSON 또는 계약 불일치는 handler 진입 초기에 거부된다. Heartbeat는 5초 주기, 10초 DELAYED, 15초 OFFLINE 기준을 사용한다. 일반 명령 응답은 3초, ESTOP 1초, recovery 30초를 기본 timeout으로 한다.

=== UART

Frame은 `SOF | VERSION | SEQUENCE | COMMAND | LENGTH | PAYLOAD | CRC16`이다. SOF는 `0xAA`, version은 `0x01`, payload 최대 128 byte, baud rate 115200, little-endian, CRC16-CCITT-FALSE를 사용한다. ACK timeout 100 ms, retry 간격 20 ms, 최대 3회, command timeout 3초가 공용 계약 상수다.

#table(
  columns: (1fr, 1fr, 1.8fr),
  inset: 4pt,
  [*역할*], [*명령 범위*], [*주요 명령*],
  [Input], [`0x10–0x1F`], [start, stop, speed, status, reset],
  [Gripper], [`0x20–0x2F`], [arm, position, home, stop, status, reset],
  [Sorting], [`0x30–0x3F`], [route, start/stop, cancel, home, status],
  [Line Tracer], [`0x40–0x4F`], [destination, position, drive, resume, status],
)

공용 PING, GET_STATUS, RESET, ACK/NACK, EVENT와 ESTOP 명령이 존재한다. ESTOP 우선순위는 계약 요구이며 실제 queue 선점·물리 정지는 역할별 구현과 실기 시험에서 검증해야 한다.

=== HTTP upload/history

업로드 endpoint는 `/api/v1/uploads/images`와 `/api/v1/uploads/logs`다. multipart file·metadata, bearer token, `Idempotency-Key == messageId`, 선언한 byte size를 요구한다. 이미지 최대 10 MiB, 로그 최대 25 MiB이며 허용 MIME과 SHA-256을 검증한다.

동일 key와 동일 metadata는 200 duplicate, 신규은 201, key 충돌은 409로 처리한다. 기타 계약 오류는 400/404/413/415/422, 내부 오류는 500으로 매핑한다. 파일은 임시 경로에 기록한 뒤 rename하고 DB 기록 실패 시 파일을 제거한다.

=== RTSP/ONVIF

Qt는 RTSP/TCP로 한 채널의 H.264 track을 수신한다. separate metadata URL이 없으면 같은 endpoint에서 ONVIF XML metadata track을 탐색한다. 응답 header 64 KiB, body 4 MiB와 설정된 video buffer 한도를 적용한다. 인증 정보는 커밋하지 않는 runtime INI에 둔다.

== 데이터 설계

=== SQLite

중앙 서버는 product, product catalog, work history, image file, device status, error log, MQTT event log, security log, HTTP upload, process runtime state와 gripper target state를 저장한다. migration filename과 checksum을 `schema_migrations`로 검증하며 변경 또는 누락 시 시작을 중단한다.

장치 registry는 재시작 후 복원되지만 pending command는 메모리 구조이므로 복원하지 않는다. SQLite는 단일 노드 저장소이며 HA, 원격 복제, 자동 backup은 구현 범위가 아니다.

=== 중복과 일관성

MQTT event는 먼저 `RECEIVED`로 기록한다. 동일 message ID가 다시 오면 duplicate count와 last received를 갱신하고 파생 저장은 반복하지 않는다. 성공은 `STORED`, 영구 파생 실패는 `REJECTED`와 사유로 남긴다. 이 중복 방지와 공정 상태기계의 메모리 처리 ID는 다른 계층이다.

=== 파일 경로와 보존

`image_root`의 MQTT image file 저장과 `upload_root`의 HTTP upload 저장은 별도 모델이다. 기본 cleanup 주기는 24시간이며 다음 기본 기간을 사용한다.

#table(
  columns: (1.5fr, 1fr, 1.5fr),
  inset: 4pt,
  [*대상*], [*기본 기간*], [*비고*],
  [MQTT event], [30일], [batch 500건],
  [Device status], [30일], [장치별 최신 snapshot 유지],
  [Image file], [30일], [파일과 metadata],
  [HTTP image/log upload], [30일], [`/uploads/images`, `/uploads/logs`],
  [Error/Security log], [180일], [운영 감사 목적],
)

파일 삭제가 실패한 metadata는 남겨 다음 주기에 재시도한다. Product, work history, product catalog, device registry는 이 retention service의 자동 삭제 대상이 아니다.

== 상태, 오류 및 복구 설계

=== 시스템 상태

시스템 상태는 `IDLE`, `RUNNING`, `STOPPED`, `ERROR`, `ESTOP`, `RECOVERY`다. STOP은 활성 work를 suspend하고 START/RESTART는 STOPPED work를 복원한다. ESTOP은 emergency suspend를 수행하며 recovery 성공 응답 뒤 STOPPED가 된다. `INITIALIZE`는 호환 명령이나 정상 독립 초기화 성공 흐름으로 사용하지 않는다.

필수 역할 장치의 connection failure, 역할별 ERROR, job ID 없는 ERROR/CRITICAL은 시스템 ERROR와 활성 work FAILED를 유발한다. 장치 ESTOP은 시스템 ESTOP로 승격한다. 이 판정은 설정된 device ID와 role mapping에 의존한다.

=== 역할별 상태 의미

공유 계약은 역할별 raw state를 idle, waiting, working, completed, stopped, emergency, error, unknown 의미로 변환한다. 알 수 없는 상태 문자열은 MQTT 1.0 호환을 위해 decode·표시는 허용하되 공정 전이, motion, 장애 승격에 사용하지 않는다. Gripper `READY`는 idle이며 중앙 완료 근거는 `COMPLETED` 또는 `PLACED`다.

=== 재시작 복구

서버 재시작 시 runtime state, active work, gripper target과 message sequence를 복원한다. 기존 RUNNING은 안전을 위해 STOPPED, ERROR/RECOVERY는 ERROR, ESTOP는 유지한다. Calibration version이 다른 target work는 무효화하고 재교정 또는 작업 재시작을 요구한다.

Device RPi의 UART reconnect는 transport 복구일 뿐이다. 장치 status, safety latch, 현재 위치와 진행 motion을 다시 조회한 뒤만 공정을 재개한다.

== 배포 및 보안 설계

- MQTT: TLS 8883, CA 검증, 계정·ACL, clean session false, keepalive 30초, reconnect backoff 1~30초
- 중앙 HTTP: bearer token 필수. example은 TLS 비활성이므로 운영망에서 reverse proxy/TLS 또는 보호된 LAN 정책을 결정한다.
- Qt 설정: 실제 password, bearer token, RTSP credential을 `control-centor.ini`에 두고 Git에 커밋하지 않는다.
- 중앙 서비스: systemd에서 broker 이후 시작, failure restart, `NoNewPrivileges`, `PrivateTmp`를 사용한다.
- 저장 경로: runtime directory는 제한 권한으로 생성하고 설정 파일은 0600을 사용한다.
- 로그: ONVIF per-frame payload logging은 UI 지연과 정보 노출 가능성이 있어 진단 중에만 켠다.

== 시험 및 추적 설계

=== 자동 시험 계층

#table(
  columns: (1.1fr, 2.7fr),
  inset: 4pt,
  [*계층*], [*주요 검증*],
  [Shared], [MQTT envelope/topic/type/state 의미, HTTP 계약, UART frame·payload·A/B/C 유효성],
  [Central], [장치·명령 manager, persistence/retention, upload, 상태기계, orchestrator, homography, 설정],
  [Qt], [dashboard 상태, 탑뷰 위치·정지/이동 화살표, recovery 색상, 단일 채널 layout, RTSP/ONVIF 한도],
  [Device RPi], [역할별 MQTT↔UART mapping, ACK/NACK, 중복·timeout·ESTOP, Vision workflow],
  [STM32 Host], [control task, sensor filter, route planner, motor·servo, safety latch와 통신 parser],
)

=== 반드시 필요한 실기 시험

- PNO-A9081RG의 선택 프로파일 H.264/RTSP/TCP 및 ONVIF metadata 호환성
- 조명·거리·각도·motion blur별 박스 ROI와 한 면 바코드 인식률
- 실제 homography 측정, mm 좌표 오차와 calibration version 교체
- Input/Sorting 속도와 sensor threshold, gate 각도, 단일 cycle 분류 성공률
- Gripper HOME/servo 보정, 물리 파지·낙하, 독립 비상 차단
- A/B/C 트랙 marker, line loss, FSR 적재·과적, 초음파 간섭과 위치 재확인
- broker·UART·전원 장애 중단과 재연결 후 안전 정지/복구
- 30일 보존 경계, 파일 삭제 실패 재시도와 디스크 부족 대응

=== 설계상 미결정 및 제약

#enum(
  [PNO-A9081RG 모델·firmware·profile은 코드에 결속되지 않으므로 배포 자산 목록으로 별도 관리한다.],
  [자동 공정과 homography의 운영 활성화 기준 및 승인 절차를 설치 현장에서 확정한다.],
  [Qt 완료 작업의 메모리 eviction 정책은 아직 없다.],
  [SQLite backup/restore와 migration rollback 운영 절차는 별도 수립이 필요하다.],
  [Gripper의 실제 위치/파지 feedback과 독립 safety power cut은 현재 구현 범위가 아니다.],
  [실제 MQTT QoS/retain, UART ESTOP 선점과 전체 장치 end-to-end 동작은 통합·실기 시험으로 확인한다.],
)

== 최종 시연 설계

#enum(
  [자동 공정을 시연할 때 `[process] enabled=true`와 필수 역할별 장치 ID를 설정하고, homography는 실측 교정값이 준비된 경우에만 별도로 활성화한다.],
  [모든 장치의 등록·heartbeat·HOME/안전 상태와 Line Tracer 현재 A/B/C 위치를 확인한다.],
  [단일 CCTV 1×1 영상과 선택적 ONVIF overlay, 중앙 서버·Qt 연결을 확인한다.],
  [상자를 투입해 work ID 생성부터 박스 ROI·바코드·상품 목적지까지 확인한다.],
  [Gripper, Sorting, Line Tracer가 같은 work ID로 이어지는지 확인한다.],
  [목적지가 정해진 정지 상태에서 흰색 경로 표식이 멈추고, `FOLLOWING_LINE`에서만 이동하는지 확인한다.],
  [도착·하역 후 완료 작업이 활성 목록에서 사라지고 이력과 로그가 남는지 확인한다.],
  [ESTOP으로 모든 공정을 정지시키고 recovery 후 STOPPED 중립 표시, 현재 위치 재확인, 명시적 재개를 검증한다.],
)

