#import "template2.typ": *

#show: project.with(title: "시스템 요구사항 명세서 (SRS)", authors: authors)

= 목적

이 문서는 Pull Request \#115로 통합한 물류 자동화 시스템의 현재 구현 기준 시스템 요구사항을 정의한다. 기준 구현은 공통 계약 모듈(`shared`), Raspberry Pi 중앙 서버(`central-server-rpi`), 장치 Raspberry Pi 노드(`device-rpi`), STM32 controller(`stm32`), Qt Control Center(`control-center`)이며, MQTT 프로토콜 버전은 1.0이다. 본 문서는 구현된 동작, 설정으로 활성화되는 동작, 실기에서 별도 검증해야 하는 동작을 구분한다.

== 문서 원칙

- ``구현``은 소스와 자동화 테스트로 확인된 동작이다.
- ``설정 필요``는 기능이 구현되어 있으나 기본 예시 설정에서 비활성 또는 환경값이 필요한 동작이다.
- ``실기 검증 필요``는 장치, broker, 네트워크, 카메라 교정 등 물리·운영 환경 없이는 검증할 수 없는 동작이다.
- 본 문서의 요구사항은 실제 계약보다 넓은 보장(예: end-to-end exactly-once, 자동 물리 복구, 고가용성)을 주장하지 않는다.

= 범위

== 포함 범위

- Central Server의 MQTT 수신·검증·명령 라우팅·장치 상태 관리
- SQLite 기반 업무 이력, 이벤트, 오류, 보안 로그, 업로드 메타데이터 저장
- HTTP 이미지·로그 업로드와 조회 API
- 선택적 공정 orchestrator와 공정 상태·재시작 복구
- MQTT 1.0, HTTP upload, UART 공통 프레임·명령 계약
- Qt Control Center의 공정 제어·상태·이력·활성 작업·top view·단일 영상/ONVIF overlay
- Input, Vision, Gripper, Sorting, LineTracer Raspberry Pi node와 STM32 controller 사이의 MQTT↔UART 제어 경계

== 제외 범위

- broker 고가용성, 원격 DB 복제·백업, 장기 재해 복구
- 다중 Control Center client별 권한·fan-out·세션 관리
- PNO-A9081RG 등 특정 카메라 모델의 구매·배포 결정과 vendor 의존 기능
- 실설비 카메라 homography 행렬의 계측, 물리 안전 인증, 모터 정지 거리·시간 보장

= 용어와 식별자

#table(
  columns: (1.1fr, 2.9fr),
  table.header([*용어*], [*정의*]),
  [Central Server], [MQTT 이벤트를 검증·저장하고 장치·Qt 클라이언트 사이의 메시지를 중계하는 Raspberry Pi 서버.],
  [Device], [``device/{deviceId}/...`` 토픽으로 식별되는 장치 노드.],
  [Qt client], [Control Center 등 ``server/request/{clientId}``로 요청하고 ``qt/{clientId}/...``로 응답을 받는 클라이언트.],
  [workId], [상품 처리 단위를 식별하는 UUID. BOX_DETECTED가 저장될 때 Central Server가 생성한다.],
  [messageId], [MQTT 이벤트와 HTTP 업로드의 멱등성 키로 쓰이는 단일 MQTT topic level 문자열.],
  [requestId], [CONTROL_COMMAND, DESTINATION_SET, EMERGENCY_STOP 및 COMMAND_RESPONSE의 명령 상관관계 식별자.],
  [공정 orchestrator], [입력·vision·gripper·sorting·line-tracer 이벤트로 업무 상태를 전이하고 다음 명령을 생성하는 선택 기능.],
  [ESTOP], [비상 정지 상태. MQTT 공정 상태 표기는 ``ESTOP``이며 UART 비상정지 명령은 ``0xF0``이다.],
)

= 운용 환경과 구성

== 논리 구성

#table(
  columns: (1.15fr, 2.85fr),
  table.header([*구성요소*], [*책임*]),
  [shared contracts], [MQTT message/topic/validation/codec, HTTP upload 계약, UART frame/CRC/parser 및 장치별 UART command 계약을 제공한다.],
  [MQTT client/transport], [Mosquitto transport를 통해 broker 접속·구독·발행을 수행한다. 실제 실행 파일은 libmosquitto 의존성이 필요하다.],
  [MqttHandler], [JSON decode, topic/message 검증, 저장, device registry 갱신, product catalog 조회, Qt·명령·공정 handler 호출을 조정한다.],
  [DeviceManager], [등록 장치의 연결 상태, heartbeat, 현재 상태, job, 오류, 라인트레이서 위치를 보관하고 registry 파일에 지속화한다.],
  [CommandManager], [명령의 대상 장치·응답·부분 실패·timeout을 집계하여 Qt 응답을 만든다.],
  [Persistence/SQLite], [업무·이벤트·상태·오류·보안·업로드 메타데이터와 공정 runtime state를 저장하고 보존 정책을 수행한다.],
  [HTTP upload server], [bearer token 기반 multipart 업로드와 이력 조회를 제공한다.],
  [ProcessOrchestrator], [feature flag가 켜진 경우 공정 상태 전이, gripper pose, 다음 장치 명령, 안전 정지·복구를 담당한다.],
  [Qt Control Center], [MQTT 공정 제어·상태·이력과 활성 작업 목록, top view, RTSP 영상 및 ONVIF metadata overlay를 제공한다.],
  [Device RPi / STM32], [Input·Vision·Gripper·Sorting·LineTracer 역할별 MQTT 명령을 UART frame으로 controller에 전달하고 controller 상태·안전 event를 MQTT로 보고한다.],
)

== 배포 및 기본 환경

- MQTT 기본 예시는 TLS 포트 8883, keepalive 30초, reconnect backoff 1~30초, persistent session(``clean_session=false``)이다.
- DB 기본 경로는 ``/var/lib/logistics/logistics.db``이고 migration 경로는 ``/usr/share/logistics/migrations``이다.
- 이미지 저장 루트와 HTTP upload 루트는 각각 ``/var/lib/logistics/images``와 ``/var/lib/logistics/uploads``로 분리된다.
- HTTP upload는 기본 예시에서 8080 포트로 활성화되어 있으나 TLS는 기본 비활성이다. 운영 환경에서는 TLS 및 bearer token을 별도 제공해야 한다.
- 실제 Central Server executable은 libmosquitto가 필요하며, HTTP 서버는 libmicrohttpd 기반이다.

= 시스템 흐름

== 수신·저장·중계 흐름

1. Central Server는 MQTT JSON을 MQTT 1.0 codec으로 해석한다.
2. topic 형식, 허용 messageType, sourceId 및 targetDeviceId 일치 여부를 검증한다.
3. 활성 공정 orchestrator가 있는 경우, 저장 전에 허용 가능한 공정 전이인지 preview한다.
4. 유효 이벤트를 SQLite 이벤트 로그에 기록하고 업무·상태·오류 등의 파생 데이터를 단일 DB transaction으로 저장한다.
5. 저장 성공 후 공정 상태를 commit하고, device registry·product catalog·Qt event/status/error/response 라우팅을 수행한다.
6. 저장 가능한 일시 오류는 25ms, 50ms, 75ms 간격으로 최대 3회 시도한다.

이 순서는 DB 저장과 MQTT publish를 하나의 분산 transaction으로 묶지 않는다. 따라서 DB 저장 성공 후 외부 publish 또는 공정 명령 publish가 실패할 수 있다.

== 업무 공정 흐름(기능 활성 시)

#table(
  columns: (1.1fr, 1.25fr, 1.65fr),
  table.header([*입력/상태*], [*상태 전이*], [*서버 동작*]),
  [BOX_DETECTED], [새 workId 생성, INPUT_DETECTED], [product를 DETECTED로 생성하고 input conveyor STOP을 먼저 보낸 뒤 WORK_CREATED를 vision 장치 및 Qt event에 QoS 1 발행한다.],
  [WORK_CREATED 발행 성공], [VISION_ASSIGNED], [vision 배정을 확정한다.],
  [POSITION_DETECTED], [VISION_PROCESSING], [homography 활성 시 box corner로 gripper target pose를 계산한다.],
  [BARCODE_DETECTED SUCCESS], [BARCODE_RECOGNIZED], [product_catalog를 조회한다.],
  [PRODUCT_INFO SUCCESS], [PRODUCT_IDENTIFIED], [destination을 기록하고 line-tracer pickup 경로 DESTINATION_SET과 gripper START/PICK 명령을 생성한다.],
  [gripper 완료 상태], [SORTING_REQUESTED], [sorting 장치에 DESTINATION_SET을 생성하고 input conveyor START를 보낸다.],
  [sorting 완료 상태], [TRANSPORT_REQUESTED], [PRODUCT_INFO 직후 할당한 line-tracer가 운송을 시작할 수 있는 상태로 전이한다.],
  [line-tracer working], [TRANSPORTING], [운송 진행 상태로 유지한다.],
  [WORK_COMPLETED SUCCESS], [COMPLETED], [활성 업무가 없으면 시스템 상태는 IDLE이 된다.],
)

공정 상태는 ``IDLE``, ``RUNNING``, ``STOPPED``, ``ERROR``, ``ESTOP``, ``RECOVERY``이며 업무 상태는 입력 감지부터 완료·실패·중지·비상정지·복구까지의 세부 단계를 가진다. 작업별 상태는 독립적으로 유지된다.

== 공정 feature flag와 안전 경계

- 기본 ``server.ini.example``에서는 ``[process] enabled=false``이다. 이 상태에서는 MQTT 저장·중계는 동작하지만 자동 공정 명령 생성·상태 전이는 활성화되지 않는다.
- ``enabled=true``에는 input, vision, gripper, sorting, line-tracer의 유효한 device ID 설정이 필요하다.
- homography는 별도 ``[homography] enabled`` flag로 제어하며 기본 비활성이다. 활성화 시 유효한 position corner와 설치별 교정값이 없으면 업무 전이를 거부한다.
- 공정 상태기계는 논리 안전 경계다. 실제 모터 정지, 센서 안전회로, 물리적 비상정지의 인증 또는 보장을 대체하지 않는다.

= 기능 요구사항

== MQTT 계약 및 장치 상태

#table(
  columns: (0.7fr, 2.1fr, 0.75fr),
  table.header([*ID*], [*요구사항*], [*상태*]),
  [SRS-MQ-001], [모든 MQTT 메시지는 ``protocolVersion=1.0``, ``messageId``, ``messageType``, ``sourceId``, ISO-8601 ``timestamp``, ``data`` envelope를 만족해야 한다. 지원하지 않는 version, type, 필수 field 누락은 거부한다.], [구현],
  [SRS-MQ-002], [시스템은 ``server/request/{clientId}``, ``qt/{clientId}/{response|status|event|error}``, ``device/{deviceId}/{register|command|response|status|event|error|heartbeat}``, ``system/broadcast/command`` 토픽 계약을 사용해야 한다.], [구현],
  [SRS-MQ-003], [topic이 endpoint를 식별하면 message의 sourceId는 endpoint와 같아야 한다. device command targetDeviceId는 command topic의 deviceId와 같아야 하며 broadcast target은 ``ALL``이어야 한다.], [구현],
  [SRS-MQ-004], [heartbeat 기준은 5초 간격, 10초 후 DELAYED, 15초 후 OFFLINE로 판정하고 상태 변경을 Qt DEVICE_STATUS로 전달해야 한다.], [구현],
  [SRS-MQ-005], [Heartbeat은 QoS 0/non-retain, error는 QoS 1/retain 허용, 그 외 일반 이벤트는 QoS 1을 기본 계약으로 사용해야 한다.], [구현],
  [SRS-MQ-006], [STATUS_REQUEST의 componentId가 ``CENTRAL_SNAPSHOT``이면 등록 장치의 최신 상태 snapshot을 Qt status 채널로 재생해야 한다.], [구현],
  [SRS-MQ-007], [CommandManager는 requestId별 대상 장치 응답을 집계하고 duplicate·late response를 구분해야 한다. 일반 command timeout은 3초, ESTOP confirmation은 1초, RECOVERY completion은 30초다.], [구현],
)

MQTT 메시지의 전달 정책은 계약의 허용 범위다. 수신 persistence 경로는 현재 transport metadata에 QoS 1과 retained=false를 기록하므로, DB 로그를 broker wire-level QoS/retain의 완전한 관측값으로 해석해서는 안 된다.

== Qt 단일 채널 인터페이스

#table(
  columns: (1.45fr, 1.25fr, 2.0fr),
  table.header([*경로*], [*방향*], [*용도*]),
  [``server/request/{qtClientId}``], [Qt → Central], [CONTROL_COMMAND, DESTINATION_SET, EMERGENCY_STOP 등 요청 수신.],
  [``qt/{qtClientId}/response``], [Central → Qt], [집계된 COMMAND_RESPONSE, 즉시 거부, dispatch 실패, timeout 결과.],
  [``qt/{qtClientId}/status``], [Central → Qt], [등록·heartbeat·status 기반 DEVICE_STATUS와 snapshot replay.],
  [``qt/{qtClientId}/event``], [Central → Qt], [WORK_CREATED 및 제품 관련 event, catalog PRODUCT_INFO.],
  [``qt/{qtClientId}/error``], [Central → Qt], [장치 ERROR_OCCURRED 및 재교정에 따른 work invalidation 알림.],
)

Qt client ID는 하나의 ``[routing] qt_client_id`` 설정값을 사용한다. 본 기준 구현은 다중 Qt client별 권한, fan-out 정책 또는 UI 세션 관리를 정의하지 않는다.

== Control Center, 영상 및 top view 요구사항

#table(
  columns: (0.7fr, 2.1fr, 0.75fr),
  table.header([*ID*], [*요구사항*], [*상태*]),
  [SRS-UI-001], [Control Center는 단일 영상 채널(1x1)을 RTSP/TCP로 수신하고, 같은 채널의 ONVIF RTSP metadata를 파싱하여 detection overlay를 영상 좌표에 표시해야 한다.], [구현],
  [SRS-UI-002], [배포 CCTV는 PNO-A9081RG 1대를 사용하되 코드가 특정 모델명에 결속되어서는 안 된다. RTSP URL과 ONVIF metadata URL은 설정으로 제공해야 한다.], [설정 필요·실기 검증 필요],
  [SRS-UI-003], [활성 작업 목록은 완료되지 않은 work만 표시해야 한다. WORK_COMPLETED는 UI 목록에서 숨기지만 CurrentProduct 및 dashboard 상태 cache의 메모리 삭제를 의미하지 않는다.], [구현],
  [SRS-UI-004], [top view는 line-tracer의 departurePosition, targetPosition, confirmedPosition(A/B/C), movementState를 표시해야 한다.], [구현],
  [SRS-UI-005], [top view의 이동 화살표와 이동 표현은 movementState=MOVING, 유효한 departure/target path, currentState=FOLLOWING_LINE일 때만 동작해야 한다. STOPPED는 정적 표현이며 이동 화살표를 표시하지 않는다.], [구현],
  [SRS-UI-006], [ESTOP recovery 완료 후 UI는 장치/공정 표시를 중립 STOPPED 상태로 전환해야 하며, 자동 운전 재개로 표현해서는 안 된다.], [구현],
)

Control Center의 RTSP/ONVIF receiver는 RTSP/TCP만 지원한다. ONVIF metadata track이 없거나 XML을 해석하지 못하면 overlay를 제공하지 않으며, 영상·metadata 연결의 실제 호환성은 배포 카메라에서 확인해야 한다.

== 제품·공정 요구사항

#table(
  columns: (0.7fr, 2.1fr, 0.75fr),
  table.header([*ID*], [*요구사항*], [*상태*]),
  [SRS-PR-001], [BOX_DETECTED가 유효하게 저장되면 Central Server는 UUID workId를 생성하고 product lifecycle을 DETECTED로 생성해야 한다. 동일 messageId 재수신은 기존 workId를 반환하는 중복 처리여야 한다.], [구현],
  [SRS-PR-002], [BARCODE_DETECTED SUCCESS는 활성 product_catalog를 조회하여 PRODUCT_INFO를 생성해야 한다. catalog miss이고 default_destination이 설정되면 ``UNREGISTERED`` 상품과 기본 목적지로 계속 처리해야 한다.], [구현],
  [SRS-PR-003], [자동 공정은 feature flag가 활성일 때만 BOX_DETECTED 후 input conveyor STOP, PRODUCT_INFO 성공 후 line-tracer pickup 경로 DESTINATION_SET과 gripper START/PICK, gripper 완료 후 sorting DESTINATION_SET과 input conveyor START를 생성해야 한다.], [구현],
  [SRS-PR-004], [PRODUCT_INFO 성공에는 non-empty destination이 필요하다. 실패한 barcode/product info 또는 명령 dispatch 실패는 해당 work를 FAILED로 만들고 시스템을 ERROR로 전이해야 한다.], [구현],
  [SRS-PR-005], [homography 활성 시 gripper 명령은 coordinateFrame, mm targetPose, box dimensions, calibrationVersion을 포함해야 한다.], [설정 필요],
  [SRS-PR-006], [line-tracer initialize 위치는 A, B, C만 허용하며 설정된 초기 위치를 명령에 포함해야 한다.], [구현],
  [SRS-PR-007], [input 초음파 장치는 측정 건전성(OK/FAULT)과 raw 거리를 전송하고 Central Server가 설정된 진입·이탈 임계값, 히스테리시스와 디바운스로 박스 진입을 판정해야 한다. input 센서 1의 DETECTED 진입은 BOX_DETECTED를 생성하며, line-tracer의 로컬 장애물 안전 정지는 별도 STM32 safety 로직을 유지한다.], [구현],
  [SRS-PR-008], [vision은 설정된 프레임 수 안에 바코드를 읽지 못하면 실패 BARCODE_DETECTED를 발행하여 해당 work가 무한 대기하지 않게 해야 한다.], [구현],
  [SRS-PR-009], [자동 공정 명령은 MQTT 발행 전에 영속 outbox에 저장해야 한다. 서버 재시작 후 안전 STOP은 연결 즉시 같은 requestId로 재발행하고 나머지 명령은 START/RESTART 성공 뒤 재발행해야 한다.], [구현],
)

== HTTP upload 및 조회 요구사항

#table(
  columns: (0.7fr, 2.1fr, 0.75fr),
  table.header([*ID*], [*요구사항*], [*상태*]),
  [SRS-HTTP-001], [이미지는 ``POST /api/v1/uploads/images``, 로그는 ``POST /api/v1/uploads/logs`` multipart 요청으로 수신해야 한다.], [구현],
  [SRS-HTTP-002], [POST는 ``Authorization: Bearer`` token, file, deviceId, messageId, sha256, byteSize 및 이미지의 workId/capturedAt 또는 로그의 startedAt/endedAt을 검증해야 한다.], [구현],
  [SRS-HTTP-003], [Idempotency-Key header는 messageId와 같아야 하며, 동일 metadata 재전송은 기존 upload를 반환하고 다른 metadata로 재사용하면 conflict여야 한다.], [구현],
  [SRS-HTTP-004], [이미지는 JPEG/PNG 및 최대 10 MiB, 로그는 plain text/gzip/zip 및 최대 25 MiB만 허용해야 한다.], [구현],
  [SRS-HTTP-005], [서버는 파일 SHA-256을 재계산하고, 임시 파일을 rename한 뒤 DB metadata transaction을 commit해야 한다. 실패 시 저장 파일을 제거해야 한다.], [구현],
  [SRS-HTTP-006], [PRODUCT_IMAGE MQTT event는 성공한 IMAGE http_upload의 uploadId, workId, deviceId, ``/uploads/`` path, checksum과 일치해야 저장할 수 있다.], [구현],
  [SRS-HTTP-007], [HTTP TLS는 설정으로 활성화하며 certificate와 private key를 읽을 수 없거나 bearer token이 비어 있으면 HTTP 서버 시작을 거부해야 한다.], [설정 필요],
)

성공 응답은 uploadId, path, checksum, duplicate를 포함한다. 오류 상태는 400, 404, 409, 413, 415, 422, 500으로 매핑한다. HTTP 업로드와 MQTT PRODUCT_IMAGE event는 두 단계 계약이며 하나의 원자적 외부 transaction이 아니다.

== UART 공통 계약

#table(
  columns: (0.7fr, 2.1fr, 0.75fr),
  table.header([*ID*], [*요구사항*], [*상태*]),
  [SRS-UART-001], [공통 frame은 ``SOF|VERSION|SEQUENCE|COMMAND|LENGTH|PAYLOAD|CRC16``이며 SOF=0xAA, version=0x01, payload 최대 128 bytes, CRC16-CCITT-FALSE, little-endian을 사용해야 한다.], [구현],
  [SRS-UART-002], [UART baudrate는 115200이며 ACK timeout 100ms, retry interval 20ms, 최대 retry 3, command timeout 3초의 공통 기준을 제공해야 한다.], [구현],
  [SRS-UART-003], [command 예약 영역은 input 0x10–0x1F, gripper 0x20–0x2F, sorting 0x30–0x3F, line-tracer 0x40–0x4F로 분리해야 한다.], [구현],
  [SRS-UART-004], [EMERGENCY_STOP command 0xF0은 일반 command queue보다 우선 처리되어야 한다.], [계약 구현·실기 검증 필요],
)

공통 UART 라이브러리는 codec/CRC/parser와 payload 검증을 제공한다. 실제 queue, retry scheduler, 장치별 물리 정지 실행은 각 장치 구현의 책임이며 본 문서 범위에서 end-to-end 검증되지 않았다.

== 역할별 Device RPi·STM32 요구사항

#table(
  columns: (0.7fr, 2.1fr, 0.75fr),
  table.header([*ID*], [*요구사항*], [*상태*]),
  [SRS-DEV-001], [Input node는 MQTT START/STOP/RECOVERY/EMERGENCY_STOP 및 입력 감지 흐름을 input UART command·event로 연결하고 controller 상태와 sensor 상태를 MQTT로 보고해야 한다.], [구현],
  [SRS-DEV-002], [Vision node는 WORK_CREATED에 대해 HSV 기반 box ROI에서 한 면의 barcode를 탐지·decode하고 POSITION_DETECTED, BARCODE_DETECTED 및 제품 영상/실패 정보를 MQTT로 보고해야 한다.], [구현],
  [SRS-DEV-003], [Vision 처리 범위는 현재 camera에 보이는 한 면 barcode와 HSV ROI 기반 처리다. 6면 전체 판독, 범용 객체 인식 또는 범용 AI 분류를 요구하지 않는다.], [구현 범위],
  [SRS-DEV-004], [Gripper node는 MQTT START/PICK 및 RECOVERY/EMERGENCY_STOP을 gripper UART command로 연결하고, controller safety event·상태를 COMMAND_RESPONSE, DEVICE_STATUS, ERROR_OCCURRED로 보고해야 한다.], [구현],
  [SRS-DEV-005], [Sorting node는 DESTINATION_SET 및 safety command를 sorting UART command로 연결하고, active work·목적지·controller 상태를 MQTT로 보고해야 한다.], [구현],
  [SRS-DEV-006], [LineTracer node는 DESTINATION_SET과 A/B/C 초기·목적 위치 계약을 UART로 전달하고 departure/target/confirmed position 및 movementState를 DEVICE_STATUS로 보고해야 한다.], [구현],
  [SRS-DEV-007], [각 device node는 EMERGENCY_STOP을 일반 명령보다 우선 처리하고, controller의 ESTOP latch 또는 recovery/reset 결과를 MQTT safety 상태로 보고해야 한다.], [구현·실기 검증 필요],
  [SRS-DEV-008], [ESTOP recovery는 controller가 안전 reset/home/중립 위치를 확인한 뒤에만 완료로 보고해야 한다. Central Server와 UI는 그 성공 후에도 STOPPED로 남기며 명시적 START/RESTART가 필요하다.], [구현·실기 검증 필요],
)

MQTT는 역할별 RPi node와 Central Server의 상위 제어 계약이고 UART는 RPi와 STM32 controller의 하위 제어 계약이다. MQTT COMMAND_RESPONSE 성공만으로 모터·그리퍼·컨베이어의 실제 안전 상태를 보장하지 않으며, UART safety event와 실기 확인이 필요하다.

= 비기능 요구사항

#table(
  columns: (0.7fr, 2.1fr, 0.75fr),
  table.header([*ID*], [*요구사항*], [*상태*]),
  [SRS-NF-001], [SQLite migration은 순번 SQL과 checksum으로 적용 이력을 검증해야 하며, 누락 또는 이미 적용된 migration checksum 변경은 서버 시작 실패로 처리해야 한다.], [구현],
  [SRS-NF-002], [mqtt_event_log는 messageId를 unique하게 보관하고 duplicate_count 및 last_received_at을 갱신하여 저장 단계의 중복 처리를 제공해야 한다.], [구현],
  [SRS-NF-003], [장치 상태, 업무 이력, 오류, 보안 로그, 업로드 메타데이터는 관계형 schema로 보관해야 한다.], [구현],
  [SRS-NF-004], [보존 작업은 설정된 cleanup interval마다 image/upload 파일과 MQTT event, 과거 device status, error, security log를 batch로 정리해야 한다.], [구현],
  [SRS-NF-005], [파일 삭제 실패 시 metadata를 유지하여 다음 보존 주기에 재시도해야 한다.], [구현],
  [SRS-NF-006], [운영 배포는 MQTT TLS 인증서·계정, HTTP bearer token, 필요 시 HTTP TLS private key를 secret으로 제공해야 한다.], [설정 필요],
  [SRS-NF-007], [Central Server 단일 SQLite 노드, broker 연결 복구, 실제 장치 동작은 현장 부하·장애 시험으로 검증해야 한다.], [실기 검증 필요],
  [SRS-NF-008], [RTSP/TCP 영상·ONVIF metadata, UART 통신, 센서, 모터, 그리퍼, sorting, line-tracer, ESTOP latch 및 recovery 중립 위치는 대상 하드웨어에서 통합 시험으로 검증해야 한다.], [실기 검증 필요],
)

기본 보존일은 MQTT event 30일, device status 30일, error/security 180일, image/upload 30일이다. device status 정리는 장치별 최신 snapshot을 보존한다. product, work history, product catalog 및 device registry에는 이 보존 서비스의 삭제 정책이 적용되지 않는다.

= 인터페이스 계약

== MQTT message type

지원 messageType은 DEVICE_REGISTER, HEARTBEAT, BOX_DETECTED, WORK_CREATED, WORK_COMPLETED, POSITION_DETECTED, BARCODE_DETECTED, PRODUCT_IMAGE, PRODUCT_INFO, DESTINATION_SET, DEVICE_STATUS, CONTROL_COMMAND, ERROR_OCCURRED, EMERGENCY_STOP, COMMAND_RESPONSE, SENSOR_STATUS다.

CONTROL_COMMAND는 START, STOP, RESTART, INITIALIZE, STATUS_REQUEST, EMERGENCY_STOP, RECOVERY, DESTINATION_SET을 정의한다. 다만 INITIALIZE는 호환성을 위해 남아 있으며 공정 상태기계에서 별도 초기화 성공 흐름으로 사용하지 않는다. STOPPED 상태에서는 duplicate 결과가 가능하고, 그 외에는 not required로 거부된다.

== 장치 연결 상태

#table(
  columns: (1.25fr, 2.75fr),
  table.header([*상태*], [*의미*]),
  [ONLINE / DELAYED / OFFLINE / RECONNECTING], [heartbeat 또는 연결 관측에 기반한 연결 상태. OFFLINE은 공정 역할 장치의 시스템 오류 원인이 될 수 있다.],
  [RTSP_ERROR / MQTT_ERROR / MQTT_AUTH_ERROR / TLS_ERROR / UART_ERROR], [연결 실패로 분류되는 오류 상태. 공정 역할 장치에서 관측되면 시스템 ERROR 전이를 유발할 수 있다.],
  [UNKNOWN], [식별되었으나 해석할 수 없는 상태.],
)

역할별 장치 상태의 의미는 input, vision, sorting, line-tracer 계약에 따라 해석된다. 등록되지 않았거나 설정에 역할이 매핑되지 않은 source의 status가 동일하게 공정 오류를 유발한다고 보장하지 않는다.

= 오류·안전 정지·복구

== 오류 처리

- MQTT JSON/계약 검증 실패는 수신을 거부하고 오류 로그를 남긴다. 파싱·검증 전에 거부된 메시지는 일반 mqtt_event_log 저장 흐름에 들어가지 않는다.
- persistence 내부에서 envelope/topic-source 불일치를 발견하면 security_log에 ``INVALID_MQTT_ENVELOPE``를 기록한다.
- device status의 connection failure 또는 역할별 ERROR, jobId 없는 ERROR/CRITICAL event는 시스템 ERROR와 활성 work FAILED를 유발한다.
- 개별 work의 barcode/product 정보 실패, WORK_COMPLETED 실패, 명령 dispatch 실패는 해당 work FAILED와 시스템 ERROR를 유발한다.
- ESTOP은 활성 work의 이전 단계를 보존하고 work를 EMERGENCY_STOPPED로 전이한다.

== 시스템 명령 전이

#table(
  columns: (1fr, 1.4fr, 1.6fr),
  table.header([*명령*], [*허용 상태*], [*결과*]),
  [START / RESTART], [IDLE 또는 STOPPED], [suspended work를 복원하고 RUNNING으로 전이하며 input·sorting conveyor가 운전하도록 전체 장치에 시작 명령을 보낸다. RUNNING이면 duplicate 결과다.],
  [STOP], [IDLE 또는 RUNNING], [active work를 suspend하고 STOPPED로 전이한다.],
  [EMERGENCY_STOP], [ESTOP 이외], [active work를 emergency suspend하고 ESTOP로 전이한다.],
  [RECOVERY], [ERROR 또는 ESTOP], [active work를 RECOVERING으로 두고 RECOVERY로 전이한다.],
  [RECOVERY SUCCESS 응답], [RECOVERY], [RECOVERING work를 STOPPED로 두고 시스템을 STOPPED로 전이한다.],
  [INITIALIZE], [호환 처리], [독립 초기화 절차로 사용하지 않는다.],
)

RECOVERY request의 성공 COMMAND_RESPONSE가 Central Server에 수신되어야 복구 완료가 확정된다. 복구 성공 뒤 업무는 자동 RUNNING으로 재개되지 않으며 STOPPED 상태로 남는다. 운영자 또는 상위 제어가 START/RESTART를 별도로 발행해야 한다.

전체 장치 응답 중 실패가 하나라도 있으면 START/RESTART/STOP/RECOVERY는 시스템 ERROR로 전이한다. EMERGENCY_STOP 실패는 중앙의 ESTOP을 해제하지 않는다. ``DUPLICATED`` 응답은 이미 같은 명령이 적용된 성공으로 집계한다.

== 서버 재시작 복구

- 서버는 system state, active work, suspended stage, destination, failure reason, gripper target, 내부 message sequence와 미완료 자동 명령 outbox를 DB에 저장한다.
- 재시작 시 저장된 RUNNING은 안전하게 STOPPED로, ERROR와 RECOVERY는 ERROR로, ESTOP는 ESTOP로 복원한다.
- RECOVERY 중 서버가 재시작되면 work는 FAILED가 되고 ``server restarted while recovery was in progress`` 사유를 남긴다.
- 재시작 복원 시 terminal FAILED work와 이에 속한 미완료 명령은 제거하여 정상 업무가 실패 업무를 기다리지 않게 한다.
- homography calibration version 또는 coordinate frame이 달라진 저장 gripper target은 무효화하며 해당 work에는 재검출이 필요하다는 오류 알림을 생성한다.
- 공정 상태기계의 메모리 processed messageId 집합은 재시작 시 비워진다. 저장 계층의 messageId 중복 방지가 재시작 후의 영속 중복 방지 기준이다.

= 검증 및 추적성

== 자동화 테스트 추적표

#table(
  columns: (1fr, 1.7fr, 1.7fr, 0.8fr),
  table.header([*요구사항*], [*구현 근거*], [*테스트 근거*], [*상태*]),
  [SRS-MQ-001~003], [Shared MQTT codec·validation·topic], [Shared contract, Central MQTT handler], [구현],
  [SRS-MQ-004~007], [Heartbeat·device·command manager], [Device/command manager, MQTT handler], [구현],
  [SRS-PR-001~004], [Persistence·state machine·orchestrator], [Storage, MQTT handler, process state/orchestrator], [구현],
  [SRS-PR-005~006], [Homography·orchestrator·command manager], [Homography, process orchestrator, command manager], [설정 필요],
  [SRS-UI-001~002], [Main window, RTSP/ONVIF, detection overlay], [RTSP H.264, ONVIF metadata, overlay], [구현/실기 검증],
  [SRS-UI-003~006], [Product panel, dashboard state, factory top view], [Product panel, dashboard state, factory top view], [구현],
  [SRS-HTTP-001~007], [HTTP contract, upload service/server], [Upload service, Unix HTTP server], [구현/설정 필요],
  [SRS-UART-001~004], [UART protocol·codec·parser], [Shared UART contracts], [계약 구현],
  [SRS-DEV-001~008], [Device RPi role nodes·common, STM32 controllers], [Role node suites, STM32 safety/control suites], [구현/실기 검증],
  [SRS-NF-001~005], [Database·persistence·migration], [Storage, process state store], [구현],
  [SRS-NF-006~008], [Runtime config와 MQTT/HTTP transport], [설정·통합·실기 시험], [실기 검증],
)

세부 구현 파일과 테스트 파일은 같은 저장소의 `shared/tests`, `central-server-rpi/tests`, `control-center/tests`, `device-rpi/tests`, `stm32/*/Tests`에서 요구사항 ID별로 확인한다.

== 검증 제한

- HTTP upload server test target은 Unix에서만 등록된다.
- 자동화 테스트는 계약, codec, 상태기계, SQLite, upload service, 설정, MQTT handler의 논리 동작을 다룬다.
- TLS handshake, broker 재접속의 실제 network 조건, 단일 RTSP/TCP·ONVIF 채널의 카메라 호환성, 장치 UART retry queue, 실제 ESTOP 정지 거리·시간과 recovery 중립 위치, 카메라 교정값, gripper/sorting/line-tracer의 물리 동작은 현장 통합 시험의 대상이다.
- MQTT persistent session은 설정과 client 구현으로 지원하되, broker 보관 정책 및 단전·망분리 상황의 delivery 보장은 broker/운영 환경 검증이 필요하다.
