# 공정·복구 신뢰성 구현 계획

> 설계 기준: `docs/plans/2026-08-15-process-recovery-fresh-reset-design.md`

**목표:** RECOVERY 성공 시 모든 진행 작업을 폐기하고 전체 노드를 초기화한 뒤 `STOPPED`에서 새 START를 기다리며, 정상 공정은 투입 정지 → 비전 → 그리퍼 HOME → 분류 → 운송 순서로 한 번씩 진행되게 한다.

**구현 원칙:** 상태 전이와 durable MQTT row를 가능한 한 같은 SQLite transaction에 저장한다. 제어/결과 메시지는 QoS 1 durable, 센서 최신값은 QoS 0 volatile로 분리한다. 각 단계는 실패 테스트를 먼저 추가하고 최소 구현으로 통과시킨 뒤 관련 테스트, 포맷, `git diff --check`, commit, push를 수행한다.

**변경 범위:** 중앙 서버, shared MQTT 계약, Pi 공통 MQTT, vision/input/gripper/sorting/line-tracer 노드. STM32 코드는 변경하지 않는다.

---

## Task 1: 기준선과 회귀 테스트 목록 고정

**파일**

- 수정: `docs/plans/2026-08-15-process-workflow-reliability-implementation-plan.md`

1. 현재 브랜치를 원격과 동기화하고 깨끗한 상태인지 확인한다.

   ```bash
   git pull --ff-only
   git status --short
   ```

2. OpenCV를 제외한 공통 기준 빌드를 만든다.

   ```bash
   cmake -S . -B build-process-recovery \
     -DBUILD_TESTING=ON \
     -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
     -DLOGISTICS_BUILD_VISION_NODE=OFF
   cmake --build build-process-recovery -j2
   ctest --test-dir build-process-recovery --output-on-failure
   ```

3. 실패가 있으면 이번 변경 전 실패인지 기록하고 구현 범위와 분리한다.

4. 문서 보완만 있을 때도 `git diff --check`를 실행하고 커밋한다.

---

## Task 2: 중앙 상태 머신의 RECOVERY 전체 폐기

**파일**

- 수정: `central-server-rpi/tests/process_state_machine_test.cpp`
- 수정: `central-server-rpi/src/process_manager/process_state_machine.cpp`
- 수정: `central-server-rpi/include/logistics/central_server/process_state_machine.hpp`

1. 실패 테스트를 추가한다.

   - `InputDetected`, `VisionAssigned`, `BarcodeRecognized`, `GripperTransferring`, `Sorting`, `Transporting` 작업을 각각 만든다.
   - ESTOP → RECOVERY → `CompleteSystemRecovery()`를 수행한다.
   - 완료 후 `Works()`가 비어 있고 system state가 `STOPPED`인지 검증한다.
   - 이후 START가 과거 suspended stage를 복원하지 않는지 검증한다.

2. 테스트를 실행해 현재 stage-aware 복원 때문에 실패하는지 확인한다.

   ```bash
   cmake --build build-process-recovery --target central_server_process_state_machine_test -j2
   ctest --test-dir build-process-recovery -R central_server_process_state_machine_test --output-on-failure
   ```

3. `CompleteSystemRecovery()`가 모든 active work를 제거하고 `STOPPED`로 전이하도록 수정한다.

4. RECOVERY 경로에서는 `RestoreSuspendedWorks()` 및 운송 단계 즉시 복원을 호출하지 않도록 제거한다. 운영자 STOP의 별도 재개 의미는 변경하지 않는다.

5. 테스트, 포맷, diff check 후 commit/push 한다.

   ```bash
   clang-format -i central-server-rpi/src/process_manager/process_state_machine.cpp \
     central-server-rpi/include/logistics/central_server/process_state_machine.hpp \
     central-server-rpi/tests/process_state_machine_test.cpp
   git diff --check
   git commit -am "fix: reset all work after system recovery"
   git push
   ```

---

## Task 3: RECOVERY 완료를 SQLite와 MQTT에 원자 반영

**파일**

- 수정: `central-server-rpi/src/application.cpp`
- 수정: `central-server-rpi/src/process_manager/process_orchestrator.cpp`
- 수정: `central-server-rpi/include/logistics/central_server/process_orchestrator.hpp`
- 수정: `central-server-rpi/src/db_manager/process_state_store.cpp`
- 수정: `central-server-rpi/include/logistics/central_server/process_state_store.hpp`
- 수정: `central-server-rpi/tests/process_orchestrator_test.cpp`
- 수정: `central-server-rpi/tests/process_state_store_test.cpp`
- 수정: `central-server-rpi/tests/process_integration_test.cpp`

1. RECOVERY 전 active work 목록을 snapshot하고 각 작업의 `WORK_COMPLETED(result=FAILED, reason=CANCELLED_BY_RECOVERY)` row를 만드는 실패 테스트를 작성한다.

2. 한 transaction에서 다음을 저장하는 store API를 추가한다.

   - system state `STOPPED`
   - empty work list
   - empty process command tracker
   - empty generic/system command snapshots
   - 복구 취소 terminal outbox rows

3. transaction 실패 시 메모리 state를 완료로 노출하지 않고 `RECOVERY`를 유지하는 테스트를 추가한다.

4. `restored_input_detected_work_ids`, `restored_vision_assigned_work_ids`를 RECOVERY 재개에 쓰는 application 경로를 제거한다. START가 이전 `WORK_CREATED`를 재생하지 않는지 통합 테스트로 검증한다.

5. RECOVERY 응답 누락/실패 시 active work와 pending command가 유지되고 Qt 응답에 누락 device가 포함되는지 검증한다.

6. 관련 테스트를 실행하고 commit/push 한다.

   ```bash
   cmake --build build-process-recovery --target \
     central_server_process_orchestrator_test \
     central_server_process_state_store_test \
     central_server_process_integration_test -j2
   ctest --test-dir build-process-recovery \
     -R "central_server_process_(orchestrator|state_store|integration)_test" \
     --output-on-failure
   git diff --check
   git commit -am "fix: atomically discard recovered process state"
   git push
   ```

---

## Task 4: 모든 Pi 노드의 RECOVERY 초기화 계약 통일

**파일**

- 수정: `device-rpi/common/device_control/device_control_state.cpp`
- 수정: `device-rpi/common/include/logistics/device/device_control_state.hpp`
- 수정: `device-rpi/input-node/main.cpp`
- 수정: `device-rpi/vision-node/main.cpp`
- 수정: `device-rpi/gripper-node/main.cpp`
- 수정: `device-rpi/sorting-node/main.cpp`
- 수정: `device-rpi/linetracer-node/main.cpp`
- 수정: `device-rpi/tests/device_control_policy_test.cpp`
- 수정: `device-rpi/tests/input_node_test.cpp`
- 수정: `device-rpi/tests/gripper_node_test.cpp`
- 수정: `device-rpi/tests/sorting_node_test.cpp`
- 수정: `device-rpi/tests/linetracer_node_test.cpp`
- 수정: `device-rpi/tests/vision_mqtt_workflow_test.cpp`

1. ESTOP-origin RECOVERY도 `preserve_work=false`, `clear_work=true`가 되는 실패 테스트를 먼저 작성한다.

2. 각 노드의 RECOVERY 성공 뒤 다음을 검증한다.

   - input: conveyor stopped, no queued work command
   - vision: assignment/workflow/result/upload/capture/job cleared
   - gripper: active cycle cleared, HOME/STOPPED
   - sorting: active cycle/destination cleared, conveyor stopped, gate home
   - line tracer: active route cleared, stopped/home state

3. RECOVERY 명령 재전달이 동일 결과를 내는 멱등 테스트를 추가한다.

4. 노드별 테스트를 실행하고 commit/push 한다.

   ```bash
   cmake --build build-process-recovery --target \
     device_control_policy_test device_input_node_test device_gripper_node_test \
     device_sorting_node_test device_linetracer_node_test -j2
   ctest --test-dir build-process-recovery \
     -R "device_(control_policy|input_node|gripper_node|sorting_node|linetracer_node)_test" \
     --output-on-failure
   git diff --check
   git commit -am "fix: clear node work on safety recovery"
   git push
   ```

---

## Task 5: fresh/resume 시작 모드 분리

**파일**

- 수정: `central-server-rpi/include/logistics/central_server/database.hpp`
- 수정: `central-server-rpi/src/server_config.cpp`
- 수정: `central-server-rpi/src/application.cpp`
- 수정: `central-server-rpi/src/db_manager/database.cpp`
- 수정: `central-server-rpi/config/server.ini.example`
- 수정: `central-server-rpi/tests/server_config_test.cpp`
- 수정: `central-server-rpi/tests/storage_test.cpp`

1. `startup_mode=fresh|resume` 파싱 테스트를 작성하고 통합 시험 기본값을 `fresh`로 둔다.

2. fresh 시작 테스트에 오래된 migration checksum, runtime work, inbox/outbox, command rows와 상품 카탈로그를 준비한다.

3. fresh 시작이 기존 DB에 migration을 적용하기 전에 DB를 교체하고, 새 schema를 적용한 뒤 catalog만 복사하는지 검증한다.

4. resume 시작은 기존 runtime/inbox/outbox를 보존하는지 검증한다.

5. 기존 `reset_on_start`는 한 번의 호환 경고 후 `startup_mode`로 교체하거나 제거한다. 두 설정이 동시에 process resume와 DB reset을 만들 수 없게 validation한다.

6. 테스트와 commit/push를 수행한다.

---

## Task 6: process epoch로 이전 MQTT 공정 차단

**파일**

- 수정: `shared/include/logistics/contracts/mqtt_message.hpp`
- 수정: `shared/include/logistics/contracts/mqtt_codec.hpp`
- 수정: `shared/tests/contracts_test.cpp`
- 수정: `central-server-rpi/src/application.cpp`
- 수정: `central-server-rpi/src/mqtt_handler/mqtt_handler.cpp`
- 수정: `central-server-rpi/src/db_manager/process_state_store.cpp`
- 수정: `device-rpi/common/mqtt_client/mqtt_message_processor.cpp`
- 수정: `device-rpi/common/mqtt_client/mqtt_node_client.cpp`
- 수정: `device-rpi/tests/mqtt_message_processor_test.cpp`
- 수정: `central-server-rpi/tests/mqtt_handler_test.cpp`

1. backward-compatible optional envelope `processEpoch` codec 테스트를 작성한다.

2. fresh 시작 때 새 UUID epoch를 만들고 runtime snapshot에 저장한다. resume은 저장된 epoch를 복원한다.

3. START/INITIALIZE 및 WORK_CREATED에 current epoch를 싣고 노드가 이후 work-scoped event/response에 같은 epoch를 붙이게 한다.

4. 중앙은 현재 epoch와 다른 공정 메시지를 `REJECTED_STALE_EPOCH`로 종결하고 process handler에 전달하지 않는다.

5. epoch가 없는 legacy work-scoped 메시지는 fresh 모드에서 거부한다. heartbeat/device status/sensor 최신값은 연결 상태용으로만 허용한다.

6. START 후 센서가 `CLEAR`를 한 번 확인하기 전에는 첫 `DETECTED` edge로 작업을 만들지 않아 broker에 남은 과거 감지를 차단한다.

7. 중앙/공통 계약/노드 MQTT 테스트 후 commit/push 한다.

---

## Task 7: 센서 텔레메트리 QoS 0 분리

**파일**

- 수정: `shared/include/logistics/contracts/mqtt_message.hpp`
- 수정: `shared/tests/contracts_test.cpp`
- 수정: `device-rpi/common/mqtt_client/mqtt_node_client.cpp`
- 수정: `central-server-rpi/src/application.cpp`
- 수정: `device-rpi/tests/mqtt_publish_spool_test.cpp`
- 수정: `central-server-rpi/tests/mqtt_client_test.cpp`

1. `PolicyFor(SENSOR_STATUS)`가 QoS 0/non-retained인지 실패 테스트를 추가한다.

2. 노드의 sensor publish가 durable spool을 사용하지 않고 즉시 volatile QoS 0 publish인지 테스트한다.

3. 중앙의 Qt sensor forward도 durable outbox를 사용하지 않고 QoS 0인지 테스트한다.

4. 명령 1건과 sensor 1만 건을 섞어 command가 sensor backlog 뒤에 대기하지 않는 테스트를 추가한다.

5. 센서 값은 계속 중앙 감지기와 Qt에 전달하되, drop 시 다음 최신값으로 자연 복구되는지 검증한다.

6. 테스트 후 commit/push 한다. 이 단계는 input/sorting Yocto 노드 바이너리 재배포가 필요하지만 STM firmware 변경은 필요 없다.

---

## Task 8: 공정 명령별 timeout과 늦은 응답 처리

**파일**

- 수정: `shared/include/logistics/contracts/mqtt_message.hpp`
- 수정: `shared/tests/contracts_test.cpp`
- 수정: `central-server-rpi/src/command_manager/command_manager.cpp`
- 수정: `central-server-rpi/tests/command_manager_test.cpp`
- 수정: `central-server-rpi/src/process_manager/process_orchestrator.cpp`
- 수정: `central-server-rpi/tests/process_orchestrator_test.cpp`

1. input STOP/START, gripper EXECUTE/HOME, sorting destination/start/stop, line tracer execute에 서로 다른 현실적인 deadline을 주는 테스트를 작성한다.

2. input STOP이 3초 뒤에도 진행 중이고 허용 deadline 안의 11초 SUCCESS는 정상 완료가 되는 회귀 테스트를 추가한다.

3. timeout은 해당 command intent를 실패시키되 unrelated active work 전체에 `failure_reason`을 복사하지 않게 수정한다.

4. terminal timeout 뒤 늦은 SUCCESS는 `LATE_RESPONSE`로 기록하고 공정 상태를 되감지 않도록 한다.

5. 실제 누락 device ID와 command를 Qt terminal response에 포함한다.

6. 테스트 후 commit/push 한다.

---

## Task 9: 비전 작업 ID·바코드 상태 결합과 wall-clock 재시도

**파일**

- 수정: `device-rpi/vision-node/vision_mqtt_workflow.hpp`
- 수정: `device-rpi/vision-node/vision_mqtt_workflow.cpp`
- 수정: `device-rpi/vision-node/main.cpp`
- 수정: `device-rpi/vision-node/vision_processing_config.cpp`
- 수정: `device-rpi/vision-node/vision_processing_config.hpp`
- 수정: `device-rpi/config/vision-node.ini.example`
- 수정: `device-rpi/tests/vision_mqtt_workflow_test.cpp`
- 수정: `device-rpi/tests/vision_processing_config_test.cpp`

1. fake monotonic clock을 주입하고 다음 실패 테스트를 작성한다.

   - box frame 뒤 barcode-only frame이 같은 work를 완료한다.
   - non-operational 상태에서 assigned work가 소비되지 않는다.
   - preassignment/barcode deadline 전에는 계속 retry한다.
   - deadline 만료 시에만 정확한 failure event를 한 번 발행한다.

2. `TakeAssignedWork()` 호출 전에 operational guard를 이동한다.

3. 마지막 confirmed box observation과 barcode observation을 독립 저장하고 same active work에서 결합한다.

4. frame counter를 `preassignment_timeout_ms`, `barcode_timeout_ms` 단조 시계 deadline으로 교체한다.

5. 결과 생성 직후 durable MQTT spool flush가 성공해야 `CompleteWork()`하도록 유지한다.

6. vision workflow/config 테스트 후 실제 Pi에서 vision target만 빌드한다. OpenCV가 없는 개발 PC에서 전체 vision 빌드를 강제하지 않는다.

7. commit/push 한다. Vision Pi 바이너리 재배포가 필요하며 STM firmware는 변경하지 않는다.

---

## Task 10: 그리퍼 HOME 단일 완료 신호와 컨베이어 순서

**파일**

- 수정: `device-rpi/gripper-node/gripper_node.cpp`
- 수정: `device-rpi/tests/gripper_node_test.cpp`
- 수정: `central-server-rpi/src/process_manager/process_orchestrator.cpp`
- 수정: `central-server-rpi/tests/process_orchestrator_test.cpp`
- 수정: `central-server-rpi/tests/process_integration_test.cpp`

1. 그리퍼가 HOME 완료 전 terminal SUCCESS를 발행하지 않는 테스트를 추가한다.

2. terminal `COMMAND_RESPONSE SUCCESS` 뒤 동일 work의 `DEVICE_STATUS COMPLETED`가 와도 공정 전이가 한 번만 적용되는 테스트를 추가한다.

3. 그리퍼 HOME 성공 처리 순서를 다음으로 고정한다.

   1. sorting destination durable enqueue/ACK
   2. sorting START durable enqueue/ACK
   3. input START durable enqueue/ACK

4. 각 ACK 전에는 다음 명령이 발행되지 않는 통합 테스트를 추가한다.

5. sorting sensor DETECTED 뒤 sorting STOP → gate recovery가 같은 cycle ID로 직렬화되는지 검증한다.

6. 라인트레이서 제외 경로는 sorting completion으로 work 완료, 포함 경로는 transport command 완료로 work 완료되는지 각각 검증한다.

7. 테스트 후 commit/push 한다.

---

## Task 11: 전체 공정 및 복구 행렬

**파일**

- 수정: `central-server-rpi/tests/process_integration_test.cpp`
- 필요 시 추가: `central-server-rpi/tests/process_recovery_integration_test.cpp`
- 수정: `central-server-rpi/CMakeLists.txt`

1. 라인트레이서 포함/제외 정상 공정 테스트를 완성한다.

2. 다음 각 단계에서 ESTOP → RECOVERY 성공 → STOPPED → 새 START를 검증한다.

   - InputDetected
   - VisionAssigned
   - BarcodeRecognized/ProductInfo
   - GripperTransferring
   - Sorting
   - Transporting

3. 모든 케이스에서 recovery 후 work/pending command/outbox가 0이고, START 전 어떤 과거 process command도 발행되지 않는지 검증한다.

4. fresh 재시작 + 이전 broker message 재수신, resume 재시작 + 같은 epoch outbox replay를 각각 검증한다.

5. barcode-only 후속 프레임, 11초 input STOP, duplicate gripper completion, delayed PUBACK, reconnect를 포함한다.

6. 중앙 및 비전 비의존 전체 테스트를 실행한다.

   ```bash
   cmake --build build-process-recovery -j2
   ctest --test-dir build-process-recovery --output-on-failure
   git diff --check
   ```

7. 테스트 결과와 미실행 하드웨어 검증을 구분해 기록하고 commit/push 한다.

---

## Task 12: 실제 장비 검증과 배포 판정

**파일**

- 수정: `docs/test/` 아래 해당 통합 시험 문서
- 필요 시 수정: Yocto recipe의 `SRCREV`

1. 중앙 서버 DB를 catalog-only fresh 상태로 시작한다.

2. input/sorting/vision Pi의 MQTT outbound/inbound spool을 새 epoch 기준으로 비우거나 격리한다.

3. 노드별 배포 범위를 확인한다.

   - 중앙 변경: 중앙 서버 재빌드/재시작
   - shared/Pi common 변경: 관련 Yocto 노드 바이너리 재빌드 및 배포
   - vision 변경: vision 노드 재빌드 및 배포
   - STM32: 재플래시 불필요

4. 실제 상자 3개로 다음을 기록한다.

   - sensor publish/central receive/STOP command/STOP ACK 시각
   - WORK_CREATED/WORK_ASSIGNED 시각
   - first box/first barcode/success event 시각
   - gripper pickup/HOME/terminal response 시각
   - sorting destination/start/detect/stop/complete 시각

5. 각 공정 단계에서 ESTOP/RECOVERY를 한 번씩 수행하고 모든 장치가 정지 초기 상태인지 확인한다.

6. 통합 시험 결과를 문서화하고 최종 format/build/test/diff review 후 commit/push 한다.

## 완료 조건

- RECOVERY 성공 뒤 중앙과 모든 노드에 이전 작업 상태가 없다.
- 중앙은 `STOPPED`이며 새 START 전에는 아무 공정도 움직이지 않는다.
- 작업 ID 대기와 바코드 출력 후 정지 문제가 재현되지 않는다.
- 그리퍼 HOME 전 input conveyor가 시작하지 않는다.
- sorting conveyor는 그리퍼 HOME 뒤 시작하고 분류 감지 뒤 정지한다.
- 이전 SQLite/MQTT 공정 데이터가 fresh 세션에 영향을 주지 않는다.
- #115 정상 공정과 #127에서 유지하기로 한 timeout/실패 발행 요구가 테스트로 보장된다.
- actionable P0/P1 리뷰 finding이 없고, 미실행 항목은 실제 하드웨어 시험으로만 제한된다.
