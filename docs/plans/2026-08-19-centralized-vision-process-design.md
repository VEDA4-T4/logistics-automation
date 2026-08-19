# 중앙 공정 소유권과 비전 측정 역할 분리 설계

## 상태

- 결정일: 2026-08-19
- 결정: 승인됨
- 적용 브랜치: `test/without-linetracer`
- 적용 기준: 라인트레이서 비활성 공정

## 목표

중앙 서버를 유일한 공정 상태·명령 순서·복구·timeout 소유자로 만들고, 비전 노드는 카메라 측정값만 전달한다. 비전은 위치와 바코드를 측정해 `POSITION_DETECTED` 및 `BARCODE_DETECTED`를 발행하지만 작업 생성이나 후속 장치 명령을 결정하지 않는다.

## 현재 문제

현재 투입 초음파는 중앙의 권위 있는 작업 생성 신호지만 비전도 `BOX_DETECTED`를 발행한다. 중앙은 비전의 `BOX_DETECTED`를 비권위 소스로 거부하므로 로그가 발생하고, 중앙 재시작 뒤 비전이 보존한 이전 작업 epoch가 새 중앙 epoch와 충돌할 수 있다. 비전 내부에는 작업 할당, 바코드 deadline, 결과 완료 및 업로드 보류 상태가 있어 중앙 공정 상태와 중복된다.

## 확정 구조

### 중앙 서버

1. 투입 초음파 `SENSOR_STATUS`를 감지한다.
2. 중앙이 작업 ID와 process epoch를 발급한다.
3. 투입 컨베이어 정지 명령을 발행하고 성공한 뒤 `WORK_CREATED`를 비전에 전달한다.
4. 비전의 위치·바코드 측정값을 작업 ID와 epoch로 검증한다.
5. 바코드 성공 시 상품 카탈로그에서 `PRODUCT_INFO`를 생성한다.
6. 그리퍼, 분류, 이송 명령을 기존 ACK 게이트에 따라 순차 발행한다.
7. timeout, 실패, RECOVERY, START/RESTART를 중앙 상태 머신에서 처리한다.
8. Qt에는 중앙 상태와 중앙이 생성한 terminal event만 전달한다.

### 비전 노드

1. 카메라 프레임과 barcode/box 위치를 계산한다.
2. 중앙에서 받은 `WORK_CREATED`는 결과 correlation을 위한 최소 작업 컨텍스트로만 보관한다.
3. 측정 결과를 `POSITION_DETECTED`, `BARCODE_DETECTED`로 발행한다.
4. 이미지 업로드가 활성화된 경우 `PRODUCT_IMAGE`를 부가 결과로 발행한다.
5. `BOX_DETECTED`를 MQTT로 발행하지 않는다.
6. 작업 생성, 그리퍼·분류·이송 명령, `WORK_COMPLETED`, 시스템 ESTOP을 결정하지 않는다.
7. 바코드 실패는 해당 작업의 실패 측정 결과로 전송하며 비전 노드 자체는 ESTOP으로 전환하지 않는다.

## 위치와 homography

`POSITION_DETECTED`는 제거하지 않는다. 중앙 homography가 활성화된 경우 위치 픽셀과 box corner가 그리퍼 목표 계산에 필요하다. 위치 이벤트는 공정 전이를 직접 일으키지 않고, 중앙이 현재 작업 단계와 epoch를 확인한 뒤 측정값으로만 사용한다.

## epoch 및 복구 계약

- 중앙의 `START`/`RESTART`/`INITIALIZE`가 새 process epoch를 설정한다.
- 노드는 새 epoch를 받으면 이전 작업 correlation, 결과 outbox, 업로드 보류 상태를 폐기한다.
- 이전 epoch의 위치·바코드·이미지 결과는 중앙에서 terminal reject되며 공정 handler를 호출하지 않는다.
- 중앙 fresh/recovery 이후에는 새 `START`와 새 `WORK_CREATED` 없이는 작업을 재개하지 않는다.
- 라인트레이서가 비활성인 현재 브랜치에서는 분류 완료 후 중앙이 작업을 완료한다.

## 변경 범위

- `device-rpi/vision-node/main.cpp`: `BOX_DETECTED` 발행 제거, 측정 결과 발행과 correlation만 유지
- `device-rpi/vision-node/vision_mqtt_workflow.*`: 작업 단계 판단을 측정 버퍼·결과 correlation으로 축소하고 epoch reset 경로 보강
- `device-rpi/tests/vision_mqtt_workflow_test.cpp`: box event 비발행 계약, 위치·바코드 결합, stale epoch reset 회귀
- `central-server-rpi/tests/process_integration_test.cpp`: 초음파→WORK_CREATED→position/barcode→catalog→gripper/sorting 흐름 회귀
- `central-server-rpi/tests/process_orchestrator_test.cpp`: position/barcode 처리와 ACK 순서 회귀 보강
- 필요할 때만 shared 계약을 수정하며 기존 `POSITION_DETECTED`·`BARCODE_DETECTED` envelope를 우선 재사용한다.

STM32, UART 명령 형식, 그리퍼·분류 노드의 장치 제어 알고리즘은 변경하지 않는다.

## 성공 기준

1. 비전이 `BOX_DETECTED`를 발행하지 않는다.
2. 투입 초음파 한 번으로 중앙 작업이 하나만 생성된다.
3. 중앙이 보낸 `WORK_CREATED` 뒤 비전의 위치·바코드 결과가 같은 work ID/epoch로 전달된다.
4. 중앙이 catalog→gripper→sorting 순서로 명령을 발행하고 각 terminal response 전에는 다음 명령을 발행하지 않는다.
5. 비전 바코드 실패가 시스템 ESTOP으로 바뀌지 않는다.
6. 중앙 fresh/recovery 뒤 이전 epoch 작업이 재생되지 않는다.
7. 라인트레이서 비활성 통합 테스트와 기존 전체 회귀 테스트가 통과한다.
