# 중앙 집중형 비전 공정 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (recommended) or superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 비전 노드는 위치·바코드 측정만 전달하고 중앙 서버가 작업 생성부터 그리퍼·분류·이송·복구까지 단일 공정 상태로 처리하도록 만든다.

**Architecture:** 투입 `SENSOR_STATUS`만 중앙의 작업 생성 입력으로 사용한다. 중앙은 기존 `WORK_CREATED` assignment로 작업 ID/epoch를 비전에 전달하고, 비전은 이 값을 correlation 용도로만 보관한 뒤 `POSITION_DETECTED`, `BARCODE_DETECTED`, 선택적 `PRODUCT_IMAGE`를 발행한다. 중앙의 기존 `ProductInfo`·ACK 게이트·공정 상태 머신을 유지하고 비전의 `BOX_DETECTED` MQTT 발행과 공정 완료 판단을 제거한다.

**Tech Stack:** C++20, nlohmann/json, MQTT contract codec, CMake/CTest, OpenCV-enabled vision executable, OpenCV-free vision workflow tests, SQLite-backed central integration harness.

**Spec:** `docs/plans/2026-08-19-centralized-vision-process-design.md`

## Global Constraints

- 라인트레이서 비활성 구성(`line_tracer_enabled=false`)을 기준으로 검증한다.
- STM32 firmware, UART wire format, gripper/sorting controller algorithms는 변경하지 않는다.
- `POSITION_DETECTED`와 `BARCODE_DETECTED` envelope를 우선 재사용하며 불필요한 shared protocol 변경을 추가하지 않는다.
- 비전 바코드 실패는 해당 work의 측정 실패 결과로만 보고하고 비전 노드에서 ESTOP을 발행하지 않는다.
- 중앙 fresh/recovery 뒤 이전 process epoch의 결과를 재생하거나 후속 명령으로 승격하지 않는다.
- 각 태스크는 `-j1`로 해당 target을 빌드하고, 마지막 통합 검증은 포맷·빌드·CTest 후 별도 커밋한다.

---

### Task 1: Vision workflow measurement-only contract

**Files:**
- Modify: `device-rpi/vision-node/vision_mqtt_workflow.hpp`
- Modify: `device-rpi/vision-node/vision_mqtt_workflow.cpp`
- Test: `device-rpi/tests/vision_mqtt_workflow_test.cpp`

**Interfaces:**
- Consumes: existing `VisionObservation`, `WorkCreatedPayload`, `AssignedVisionWork`.
- Produces: `VisionMqttWorkflow::Observe(std::optional<VisionObservation>)` with no MQTT message return; `AssignWork`, `TakeAssignedWork`, `CompleteWork`, and `Reset` remain the correlation/measurement API.

- [ ] **Step 1: Write the failing test**

  Replace every workflow call that expects a returned `BOX_DETECTED` message with a direct observation call and add this assertion sequence:

  ```cpp
  vision::VisionMqttWorkflow workflow("PI-VISION-01", 2, 1);
  workflow.Observe(Observation());
  workflow.Observe(Observation());
  assert(workflow.AssignWork(WorkCreated()));
  workflow.Observe(Observation("8801234567893", true));
  const auto assigned = workflow.TakeAssignedWork();
  assert(assigned.has_value());
  assert(assigned->work_id == kWorkId);
  assert(assigned->observation->barcode == "8801234567893");
  ```

  Keep the existing barcode-before-box, barcode-only-after-box, timeout, duplicate assignment, and reset assertions, but remove any assertion that a vision box MQTT envelope exists.

- [ ] **Step 2: Run the focused test to verify it fails**

  Run:

  ```bash
  cmake --build build-process-recovery --target vision_mqtt_workflow_test -j1
  ```

  Expected: compile failure because `Observe` still requires message ID/timestamp and returns an optional MQTT message.

- [ ] **Step 3: Implement the minimal workflow API change**

  Change the declaration and definition to:

  ```cpp
  void Observe(std::optional<VisionObservation> observation);
  ```

  Preserve the existing confirmation, barcode buffering, preassignment deadline, and clear-frame behavior. Remove only the `MqttMessage` construction and message ID/timestamp parameters. The method must still transition to `kAwaitingWork` after a confirmed local box so a later central `WORK_CREATED` can attach the work ID.

- [ ] **Step 4: Run the focused test to verify it passes**

  Run:

  ```bash
  cmake --build build-process-recovery --target vision_mqtt_workflow_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^vision_mqtt_workflow_test$'
  ```

  Expected: PASS, with no test constructing a publishable vision `BOX_DETECTED` event.

- [ ] **Step 5: Commit the isolated workflow change**

  ```bash
  git add device-rpi/vision-node/vision_mqtt_workflow.hpp \
          device-rpi/vision-node/vision_mqtt_workflow.cpp \
          device-rpi/tests/vision_mqtt_workflow_test.cpp
  git commit -m "refactor: keep vision workflow focused on measurements"
  ```

### Task 2: Remove vision work creation publication

**Files:**
- Modify: `device-rpi/vision-node/main.cpp:840-866`
- Test: `device-rpi/tests/vision_mqtt_workflow_test.cpp` (extend the measurement-only contract if a helper is extracted)

**Interfaces:**
- Consumes: `VisionMqttWorkflow::Observe`, `TakeAssignedWork`, existing `MqttNodeClient` publishers.
- Produces: camera loop that never calls `PublishEvent` with `MessageType::kBoxDetected`; result publication remains `POSITION_DETECTED`, `BARCODE_DETECTED`, optional `PRODUCT_IMAGE`, and vision errors.

- [ ] **Step 1: Write the failing regression test**

  Add a pure outbound classification helper only if needed to test the main-loop decision without OpenCV. The expected behavior is:

  ```cpp
  workflow.Observe(Observation());
  assert(workflow.AssignWork(WorkCreated()));
  workflow.Observe(Observation("8801234567893", true));
  const auto work = workflow.TakeAssignedWork();
  assert(work.has_value());
  assert(MakePositionDetectedMessage("PI-VISION-01", *work, "POSITION-1", "2026-08-19T00:00:00Z")
             .message_type == mqtt::MessageType::kPositionDetected);
  assert(MakeBarcodeDetectedMessage("PI-VISION-01", *work, "BARCODE-1", "2026-08-19T00:00:00Z")
             .message_type == mqtt::MessageType::kBarcodeDetected);
  ```

- [ ] **Step 2: Run the focused test to verify it fails or exposes the old call site**

  Run:

  ```bash
  cmake --build build-process-recovery --target vision_mqtt_workflow_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^vision_mqtt_workflow_test$'
  ```

  Expected before the production edit: the workflow test may compile after Task 1, but static inspection still finds the main loop publishing the optional box event. Treat the production call site as the failing regression until it is removed.

- [ ] **Step 3: Remove only the box-event publish branch**

  In `main.cpp`, call `mqtt_workflow.Observe(std::move(observation))` without constructing a message ID/timestamp and without calling `mqtt_client.PublishEvent` for a returned box event. Keep `pending_capture.Observe` and the later assigned-work result path. Keep `WORK_CREATED` handling so the central-issued work ID remains available for result correlation.

  Do not remove image upload, position measurement, barcode fallback, or local frame buffering in this task. Those are measurement/reporting responsibilities, not process orchestration.

- [ ] **Step 4: Verify no vision BOX_DETECTED publish remains**

  Run:

  ```bash
  rg -n "PublishEvent\(|kBoxDetected|BOX_DETECTED" device-rpi/vision-node/main.cpp device-rpi/vision-node/vision_mqtt_workflow.*
  ```

  Expected: no `kBoxDetected` construction or `PublishEvent` call for a box event in the vision path; existing contract enum names in unrelated code are allowed.

- [ ] **Step 5: Build the vision workflow target and record the OpenCV validation boundary**

  Run the OpenCV-free target:

  ```bash
  cmake --build build-process-recovery --target vision_mqtt_workflow_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^vision_mqtt_workflow_test$'
  ```

  If OpenCV is available on the target host, also run:

  ```bash
  cmake --build build-process-recovery --target logistics_vision_node -j1
  ```

  Record separately if the host lacks OpenCV; do not treat an OpenCV-free workflow pass as a full vision executable build.

- [ ] **Step 6: Commit the vision publication change**

  ```bash
  git add device-rpi/vision-node/main.cpp
  git commit -m "refactor: make vision publish measurements only"
  ```

### Task 3: Central ultrasonic-to-measurement integration regression

**Files:**
- Modify: `central-server-rpi/tests/process_integration_test.cpp`
- Modify only if the regression reveals a real gap: `central-server-rpi/src/application.cpp` or `central-server-rpi/src/process_manager/process_orchestrator.cpp`

**Interfaces:**
- Consumes: central input `SENSOR_STATUS` work creation, existing `WORK_CREATED` delivery, vision `POSITION_DETECTED` and `BARCODE_DETECTED`.
- Produces: one central work and the existing ordered gripper/sorting command sequence without any vision `BOX_DETECTED` input.

- [ ] **Step 1: Add a failing integration scenario**

  Add a harness helper that feeds the input sensor detection path (three `SENSOR_STATUS` readings with `detection_status="DETECTED"`) and then feeds only vision position/barcode. Assert:

  ```cpp
  assert(harness.DetectInputUltrasonic());
  assert(harness.CountWorkCreatedDeliveries() == 1);
  assert(harness.DetectPosition());
  assert(harness.DetectBarcode());
  assert(harness.CountControlCommands(kGripperId, mqtt::ControlCommand::kExecute) == 1);
  assert(harness.CountMessages(mqtt::MessageType::kBoxDetected, kVisionId) == 0);
  ```

  Use the existing harness persistence and published-message inspection; do not add a second fake process state machine.

- [ ] **Step 2: Run the integration test to verify the old flow fails**

  ```bash
  cmake --build build-process-recovery --target central_server_process_integration_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^central_server_process_integration_test$'
  ```

  Expected before the harness/application adjustment: the sensor helper is absent or the harness cannot assert the central-created work without a direct `BOX_DETECTED` injection.

- [ ] **Step 3: Implement the smallest central test seam**

  Route the synthetic input `SENSOR_STATUS` through the same `InputDetectionGate` behavior used by `application.cpp`. The helper must preserve the existing three-sample debounce and create only one work. Do not relax `MqttHandler::SetWorkCreationSourceGuard`; vision `BOX_DETECTED` must remain rejected as non-authoritative.

  If production code changes are required, keep them limited to ensuring the already-existing input sensor path creates `WORK_CREATED` before the vision measurement events are accepted. Do not add a second work creation source.

- [ ] **Step 4: Verify the ordered central flow**

  ```bash
  cmake --build build-process-recovery --target central_server_process_integration_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^central_server_process_integration_test$'
  ```

  Expected: one input STOP, one `WORK_CREATED` to vision, one gripper execute after catalog lookup, and no vision `BOX_DETECTED` delivery. Existing ACK-gated sorting/input assertions must continue to pass.

- [ ] **Step 5: Commit the central integration regression**

  ```bash
  git add central-server-rpi/tests/process_integration_test.cpp central-server-rpi/src/application.cpp central-server-rpi/src/process_manager/process_orchestrator.cpp
  git commit -m "test: drive vision work from input sensor"
  ```

  Only include the production files if Step 3 required a production fix; otherwise stage the test file alone.

### Task 4: Epoch/recovery and non-ESTOP vision failure regression

**Files:**
- Modify: `device-rpi/tests/vision_mqtt_workflow_test.cpp`
- Modify: `central-server-rpi/tests/process_integration_test.cpp`
- Modify only if tests expose a gap: `device-rpi/vision-node/main.cpp` or `central-server-rpi/src/application.cpp`

**Interfaces:**
- Consumes: existing process epoch stamping, `DeviceControlState` clear-work decisions, central recovery commit.
- Produces: stale measurement rejection, local vision reset, and barcode failure that does not emit a vision ESTOP.

- [ ] **Step 1: Write the failing regression cases**

  Add assertions for:

  ```cpp
  // A reset clears the local correlation and no old result can be taken.
  workflow.AssignWork(WorkCreated(kWorkId));
  workflow.Reset();
  assert(!workflow.TakeAssignedWork().has_value());
  assert(workflow.AssignWork(WorkCreated("7f9f3e13-2b42-4a31-b3a2-4b8e6d9af011")));

  // A barcode failure is a result event, not an ESTOP command.
  const vision::AssignedVisionWork failed_work{ .work_id = std::string(kWorkId), .observation = Observation() };
  const auto failed = vision::MakeBarcodeDetectedMessage(
      "PI-VISION-01", failed_work, "BARCODE-FAIL", "2026-08-19T00:00:00Z");
  assert(mqtt::GetPayload<mqtt::BarcodeDetectedPayload>(failed)->recognition_status == "FAILED");
  ```

  In the central integration harness, send an old-epoch vision result after a fresh/recovery epoch and assert it is terminally rejected without a gripper command or process transition.

- [ ] **Step 2: Run the focused regression targets**

  ```bash
  cmake --build build-process-recovery --target vision_mqtt_workflow_test central_server_process_integration_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^(vision_mqtt_workflow_test|central_server_process_integration_test)$'
  ```

  Expected before the reset/epoch guard is complete: stale local correlation or old epoch result can remain available to the workflow/central process.

- [ ] **Step 3: Verify only the existing reset/epoch guard is required**

  Reuse the existing `Reset` callback in the vision MQTT command handler for `START`/`RESTART`/`INITIALIZE` and existing central stale-epoch rejection. Do not add a new persistent queue or work-state database to the vision node. A stale result must be discarded, not converted into `ERROR_OCCURRED`/ESTOP. The existing guards and reset tests satisfy this contract, so no production change is required unless a focused test exposes a regression.

- [ ] **Step 4: Re-run focused tests**

  ```bash
  cmake --build build-process-recovery --target vision_mqtt_workflow_test central_server_process_integration_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^(vision_mqtt_workflow_test|central_server_process_integration_test)$'
  ```

- [ ] **Step 5: Commit the recovery regression**

  ```bash
  git add central-server-rpi/tests/process_integration_test.cpp
  git commit -m "test: keep vision barcode failures out of estop"
  ```

### Task 5: Documentation and final verification

**Files:**
- Modify: `device-rpi/vision-node/README.md`
- Modify: `docs/plans/2026-08-19-centralized-vision-process-design.md` only if an accepted behavior needs wording clarification

**Interfaces:**
- Consumes: final code behavior and test commands from Tasks 1–4.
- Produces: operator documentation that says input ultrasonic creates work and vision reports measurements only.

- [ ] **Step 1: Update the runtime sequence documentation**

  Replace the old sequence that says vision publishes `BOX_DETECTED` with:

  ```text
  1. The input ultrasonic detection is accepted by the central server.
  2. The central server stops the input conveyor and publishes WORK_CREATED to vision.
  3. Vision measures the box position and barcode and publishes POSITION_DETECTED/BARCODE_DETECTED.
  4. The central server performs catalog lookup and controls gripper, sorting, and transport.
  ```

  Keep the warning that vision does not own work creation and does not emit process completion.

- [ ] **Step 2: Run formatting and focused/full validation**

  ```bash
  clang-format-18 --dry-run --Werror device-rpi/vision-node/vision_mqtt_workflow.hpp \
      device-rpi/vision-node/vision_mqtt_workflow.cpp device-rpi/vision-node/main.cpp \
      device-rpi/tests/vision_mqtt_workflow_test.cpp central-server-rpi/tests/process_integration_test.cpp
  cmake --build build-process-recovery --target vision_mqtt_workflow_test central_server_process_integration_test -j1
  ctest --test-dir build-process-recovery --output-on-failure -R '^(vision_mqtt_workflow_test|central_server_process_integration_test)$'
  git diff --check
  ```

  When OpenCV and the Pi toolchain are available, also build `logistics_vision_node`. The OpenCV-free workflow pass must not be reported as the full vision executable validation.

- [ ] **Step 3: Run the broader regression suite**

  ```bash
  cmake --build build-process-recovery -j1
  ctest --test-dir build-process-recovery --output-on-failure
  ```

  Expected: all configured tests pass and no line-tracer target is required for the selected branch.

- [ ] **Step 4: Commit documentation and final changes**

  ```bash
  git add device-rpi/vision-node/README.md docs/plans/2026-08-19-centralized-vision-process-design.md docs/plans/2026-08-19-centralized-vision-process-implementation-plan.md
  git commit -m "docs: define centralized vision measurement flow"
  ```
