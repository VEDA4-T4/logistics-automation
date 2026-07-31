# PR 66 Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor PR 66's process restoration, calibration invalidation reporting, and staged barcode fallback code without changing protocol, configuration, persistence schema, or runtime behavior.

**Architecture:** Return process-restoration effects as an explicit value instead of retaining a side channel in the orchestrator. Represent persisted and published invalidations with one typed value, then reduce `DetectionModule::Process` to orchestration over three private fallback helpers while retaining ownership of OpenCV state.

**Tech Stack:** C++20, CMake/CTest, SQLite3, nlohmann JSON, MQTT contracts, OpenCV 4.10 barcode/DNN modules.

## Global Constraints

- Keep the MQTT protocol, topic names, message fields, and QoS unchanged.
- Keep all INI keys, defaults, validation ranges, and example values unchanged.
- Do not add or modify a database migration.
- Preserve homography calculations, calibration version semantics, SR algorithms, thresholds, model formats, benchmark CSV columns, and failure-frame retention.
- Use commit subjects in `type: message` form without a scope.
- Do not stage the workspace-root `AGENTS.md`.
- Use GCC 10-compatible C++20; do not use floating-point `std::from_chars`.

---

## File Responsibility Map

- `central-server-rpi/include/logistics/central_server/process_orchestrator.hpp`: process restoration result and orchestrator interface.
- `central-server-rpi/src/process_manager/process_orchestrator.cpp`: filtering restored work and gripper targets.
- `central-server-rpi/include/logistics/central_server/work_invalidation.hpp`: typed work invalidation and MQTT error factory.
- `central-server-rpi/src/process_manager/work_invalidation.cpp`: MQTT `ERROR_OCCURRED` construction.
- `central-server-rpi/include/logistics/central_server/persistence.hpp`: persistence service interface consuming `WorkInvalidation`.
- `central-server-rpi/src/db_manager/persistence.cpp`: atomic, idempotent invalidation storage.
- `central-server-rpi/src/application.cpp`: startup sequencing and MQTT retry ownership.
- `device-rpi/vision-node/detection.hpp`: private staged fallback helper declarations.
- `device-rpi/vision-node/detection.cpp`: staged barcode detection and decode orchestration.
- Existing central-server and vision test files remain the behavior boundary; no new framework or service class is introduced.

---

### Task 1: Return Process Restore Results Explicitly

**Files:**
- Modify: `central-server-rpi/include/logistics/central_server/process_orchestrator.hpp`
- Modify: `central-server-rpi/src/process_manager/process_orchestrator.cpp`
- Modify: `central-server-rpi/src/application.cpp`
- Test: `central-server-rpi/tests/process_orchestrator_test.cpp`

**Interfaces:**
- Consumes: `ProcessSystemState`, `WorkProcessSnapshot`, `GripperTarget`.
- Produces:

```cpp
struct ProcessRestoreResult final {
    bool restored{ false };
    std::vector<InvalidatedRestoredWork> invalidated_works;
};

[[nodiscard]] ProcessRestoreResult RestoreAfterServerRestart(
    ProcessSystemState stored_state,
    std::vector<WorkProcessSnapshot> works,
    std::unordered_map<std::string, GripperTarget> gripper_targets,
    std::uint64_t message_sequence);
```

- Removes: `InvalidatedRestoredWorks()` and `invalidated_restored_works_`.

- [ ] **Step 1: Change the stale-calibration test to consume the result**

Replace the restore assertions in `TestChangedCalibrationDiscardsRestoredTarget` with:

```cpp
const auto restore = orchestrator.RestoreAfterServerRestart(
    central_server::ProcessSystemState::kRunning, std::move(works), std::move(targets), 12);
assert(restore.restored);
assert(restore.invalidated_works.size() == 1);
assert(restore.invalidated_works.front().work_id == kWorkId);
assert(orchestrator.GripperTargets().empty());
assert(!orchestrator.StateMachine().FindWork(kWorkId).has_value());
```

For `TestRestoredHomographyTargetCreatesGripperCommand` and `TestDisabledHomographyDiscardsRestoredTarget`, store the
return value and assert both `restored` and an empty `invalidated_works` list.

- [ ] **Step 2: Run the orchestrator test and verify compilation fails**

Run:

```bash
cmake --build build --target central_server_process_orchestrator_test --parallel 2
```

Expected: compilation fails because the existing return type is `bool` and has no `restored` or
`invalidated_works` members.

- [ ] **Step 3: Add the explicit result type and remove retained result state**

In `process_orchestrator.hpp`, place `ProcessRestoreResult` immediately after `InvalidatedRestoredWork`, change the
method return type, then delete:

```cpp
[[nodiscard]] const std::vector<InvalidatedRestoredWork>& InvalidatedRestoredWorks() const noexcept;
std::vector<InvalidatedRestoredWork> invalidated_restored_works_;
```

In `process_orchestrator.cpp`, keep `invalidated_works` local. Return:

```cpp
if (!state_machine_.RestoreAfterServerRestart(stored_state, std::move(works))) {
    return {};
}
gripper_targets_ = std::move(gripper_targets);
message_sequence_ = message_sequence;
++revision_;
return {
    .restored = true,
    .invalidated_works = std::move(invalidated_works),
};
```

Delete the `InvalidatedRestoredWorks()` definition.

- [ ] **Step 4: Update application startup to own the returned invalidations**

Before loading stored state, declare:

```cpp
std::vector<InvalidatedRestoredWork> invalidated_restored_works;
```

Replace the boolean restore call with:

```cpp
if (stored_process_state.has_value()) {
    auto restore = process_orchestrator.RestoreAfterServerRestart(
        stored_process_state->system_state,
        std::move(stored_process_state->works),
        std::move(stored_process_state->gripper_targets),
        stored_process_state->message_sequence);
    if (!restore.restored) {
        std::cerr << "[server][ERROR] stored process state is invalid\n";
        return 5;
    }
    invalidated_restored_works = std::move(restore.invalidated_works);
}
```

Replace all calls to `process_orchestrator.InvalidatedRestoredWorks()` with `invalidated_restored_works`. Capture that
local vector by reference in the notification lambda.

- [ ] **Step 5: Build and run the affected test**

Run:

```bash
cmake --build build --target central_server_process_orchestrator_test logistics_central_server --parallel 2
ctest --test-dir build --output-on-failure -R '^central_server_process_orchestrator_test$'
```

Expected: both targets build and the single selected test passes.

- [ ] **Step 6: Format and commit**

Run:

```bash
clang-format -i \
  central-server-rpi/include/logistics/central_server/process_orchestrator.hpp \
  central-server-rpi/src/process_manager/process_orchestrator.cpp \
  central-server-rpi/src/application.cpp \
  central-server-rpi/tests/process_orchestrator_test.cpp
git diff --check
git add \
  central-server-rpi/include/logistics/central_server/process_orchestrator.hpp \
  central-server-rpi/src/process_manager/process_orchestrator.cpp \
  central-server-rpi/src/application.cpp \
  central-server-rpi/tests/process_orchestrator_test.cpp
git commit -m "refactor: return process restore results explicitly"
```

---

### Task 2: Encapsulate Work Invalidation Reporting

**Files:**
- Create: `central-server-rpi/include/logistics/central_server/work_invalidation.hpp`
- Create: `central-server-rpi/src/process_manager/work_invalidation.cpp`
- Modify: `central-server-rpi/CMakeLists.txt`
- Modify: `central-server-rpi/include/logistics/central_server/persistence.hpp`
- Modify: `central-server-rpi/src/db_manager/persistence.cpp`
- Modify: `central-server-rpi/src/application.cpp`
- Test: `central-server-rpi/tests/storage_test.cpp`

**Interfaces:**
- Consumes: `contracts::mqtt::MqttMessage`, `ErrorOccurredPayload`, `Database`, and the invalidated restore records from
  Task 1.
- Produces:

```cpp
struct WorkInvalidation final {
    std::string work_id;
    std::string message_id;
    std::string error_code;
    std::string reason;
    std::string cause;
    std::int64_t occurred_at_ms{};
};

[[nodiscard]] contracts::mqtt::MqttMessage MakeWorkInvalidationError(
    std::string_view source_id,
    const WorkInvalidation& invalidation,
    std::string timestamp);

[[nodiscard]] DatabaseStatus PersistenceService::RecordWorkInvalidation(
    const WorkInvalidation& invalidation);
```

- Removes: `PersistenceService::InvalidateWork(std::string_view, std::string_view, std::string_view, std::int64_t)`.

- [ ] **Step 1: Change the storage test to use the typed invalidation**

Add the include:

```cpp
#include "logistics/central_server/work_invalidation.hpp"
#include "logistics/contracts/mqtt_topic.hpp"
#include "logistics/contracts/mqtt_validation.hpp"
```

Replace both positional invalidation calls with:

```cpp
const server::WorkInvalidation invalidation{
    .work_id = invalidated_work_id,
    .message_id = "RECALIBRATION-" + invalidated_work_id,
    .error_code = "ERR-PROCESS-RECALIBRATION-REQUIRED",
    .reason = "stored gripper target uses stale homography calibration",
    .cause = "CALIBRATION_CHANGED",
    .occurred_at_ms = base_time + 7,
};
assert(persistence.RecordWorkInvalidation(invalidation).ok());
assert(persistence.RecordWorkInvalidation(invalidation).ok());
```

Then add MQTT assertions:

```cpp
const auto error = server::MakeWorkInvalidationError("central-server", invalidation, "2026-07-31T00:00:00Z");
assert(error.message_id == invalidation.message_id);
const auto* error_payload = mqtt::GetPayload<mqtt::ErrorOccurredPayload>(error);
assert(error_payload != nullptr);
assert(error_payload->job_id == invalidation.work_id);
assert(error_payload->error_code == invalidation.error_code);
assert(error_payload->error_level == "ERROR");
assert(error_payload->current_state == "RECALIBRATION_REQUIRED");
assert(error_payload->message == invalidation.reason);
assert(mqtt::ValidateTopicMessage(mqtt::QtErrorTopic("control-center"), error).IsSuccess());
```

- [ ] **Step 2: Run the storage test build and verify compilation fails**

Run:

```bash
cmake --build build --target central_storage_test --parallel 2
```

Expected: compilation fails because `WorkInvalidation`, `RecordWorkInvalidation`, and
`MakeWorkInvalidationError` do not exist.

- [ ] **Step 3: Add the shared invalidation type and error factory**

Create `work_invalidation.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "logistics/contracts/mqtt_codec.hpp"

namespace logistics::central_server {

struct WorkInvalidation final {
    std::string work_id;
    std::string message_id;
    std::string error_code;
    std::string reason;
    std::string cause;
    std::int64_t occurred_at_ms{};
};

[[nodiscard]] contracts::mqtt::MqttMessage MakeWorkInvalidationError(
    std::string_view source_id,
    const WorkInvalidation& invalidation,
    std::string timestamp);

}  // namespace logistics::central_server
```

Implement `MakeWorkInvalidationError` in `work_invalidation.cpp` with:

```cpp
return {
    .protocol_version = std::string(contracts::mqtt::kCurrentProtocolVersion),
    .message_id = invalidation.message_id,
    .message_type = contracts::mqtt::MessageType::kErrorOccurred,
    .source_id = std::string(source_id),
    .timestamp = std::move(timestamp),
    .data = contracts::mqtt::ErrorOccurredPayload{
        .job_id = invalidation.work_id,
        .error_code = invalidation.error_code,
        .error_level = "ERROR",
        .current_state = "RECALIBRATION_REQUIRED",
        .message = invalidation.reason,
        .distance = std::nullopt,
    },
};
```

Add `src/process_manager/work_invalidation.cpp` to `logistics_central_storage`. This keeps one implementation linked
by the server and `central_storage_test` without adding a new library or service class.

- [ ] **Step 4: Replace the persistence positional API**

Include `work_invalidation.hpp` from `persistence.hpp`, declare `RecordWorkInvalidation`, and delete the positional
method.

In `persistence.cpp`, validate:

```cpp
if (!IsUuid(invalidation.work_id) || invalidation.message_id.empty() ||
    invalidation.error_code.empty() || invalidation.reason.empty() ||
    invalidation.cause.empty() || invalidation.occurred_at_ms < 0) {
    return { DatabaseStatusCode::kInvalidArgument, "invalid work invalidation metadata" };
}
```

Use `invalidation.message_id` for idempotence and:

```cpp
const EventPayload payload{
    .work_id = invalidation.work_id,
    .process_state = "ERROR",
    .error_code = invalidation.error_code,
    .severity = "ERROR",
    .error_message = invalidation.reason,
    .details_json = contracts::mqtt::Json{ { "cause", invalidation.cause } }.dump(),
};
```

Keep the existing transaction, product `ERROR` update, duplicate lookup, work-history append, error-log append, and
commit ordering unchanged.

- [ ] **Step 5: Build typed invalidations once during startup**

In `application.cpp`, convert Task 1's restore records immediately:

```cpp
std::vector<WorkInvalidation> work_invalidations;
work_invalidations.reserve(invalidated_restored_works.size());
for (const auto& restored : invalidated_restored_works) {
    work_invalidations.push_back({
        .work_id = restored.work_id,
        .message_id = "RECALIBRATION-" + restored.work_id,
        .error_code = "ERR-PROCESS-RECALIBRATION-REQUIRED",
        .reason = restored.reason,
        .cause = "CALIBRATION_CHANGED",
        .occurred_at_ms = CurrentUnixTimeMilliseconds(),
    });
}
```

Persist each value with `RecordWorkInvalidation`. In the retry lambda, replace the aggregate MQTT construction with:

```cpp
const auto error = MakeWorkInvalidationError("central-server", invalidation, CurrentIso8601Timestamp());
```

Keep the existing Qt topic, QoS, connection guard, pending flag, and retry loop.

- [ ] **Step 6: Build and run central-server tests**

Run:

```bash
cmake --build build --target \
  central_storage_test \
  central_server_process_orchestrator_test \
  logistics_central_server \
  --parallel 2
ctest --test-dir build --output-on-failure \
  -R '^(central_storage_test|central_server_process_orchestrator_test)$'
```

Expected: all three targets build and both selected tests pass.

- [ ] **Step 7: Format and commit**

Run:

```bash
clang-format -i \
  central-server-rpi/include/logistics/central_server/work_invalidation.hpp \
  central-server-rpi/src/process_manager/work_invalidation.cpp \
  central-server-rpi/include/logistics/central_server/persistence.hpp \
  central-server-rpi/src/db_manager/persistence.cpp \
  central-server-rpi/src/application.cpp \
  central-server-rpi/tests/storage_test.cpp
git diff --check
git add \
  central-server-rpi/CMakeLists.txt \
  central-server-rpi/include/logistics/central_server/work_invalidation.hpp \
  central-server-rpi/src/process_manager/work_invalidation.cpp \
  central-server-rpi/include/logistics/central_server/persistence.hpp \
  central-server-rpi/src/db_manager/persistence.cpp \
  central-server-rpi/src/application.cpp \
  central-server-rpi/tests/storage_test.cpp
git commit -m "refactor: encapsulate work invalidation reporting"
```

---

### Task 3: Isolate Staged Barcode Fallbacks

**Files:**
- Modify: `device-rpi/vision-node/detection.hpp`
- Modify: `device-rpi/vision-node/detection.cpp`
- Test: `device-rpi/tests/vision_detection_test.cpp`
- Test: `device-rpi/tests/vision_super_resolution_preview_test.cpp`

**Interfaces:**
- Consumes: existing `VisionProcessingConfig`, `DetectionDiagnostics`, barcode detector, DNN model, ROI and corner
  values.
- Produces these private `DetectionModule` helpers:

```cpp
[[nodiscard]] bool DetectBarcodeRegionsWithFallback(
    const cv::Mat& box_roi,
    bool allow_expensive_fallback,
    bool reached_failure_threshold,
    std::vector<cv::Point2f>& corners,
    DetectionDiagnostics& diagnostics);

[[nodiscard]] cv::Mat PrepareBarcodeDecodeRoi(
    const cv::Mat& box_roi,
    const std::vector<cv::Point2f>& corners,
    std::size_t barcode_index,
    DetectionDiagnostics& diagnostics) const;

void DecodeBarcodeCandidate(
    const cv::Mat& decode_roi,
    const std::vector<cv::Point2f>& selected_corners,
    const cv::Point2f& frame_offset,
    std::vector<DetectedBarcode>& barcodes,
    DetectionDiagnostics& diagnostics);
```

- `DetectionModule::Process` retains box detection, failure-counter updates, helper sequencing, success reset, and
  final result construction.

- [ ] **Step 1: Strengthen public-behavior characterization checks**

In `vision_detection_test.cpp`, add:

```cpp
void TestNoBoxResetsFallbackStateWithoutDiagnostics() {
    vision::VisionProcessingConfig config;
    config.super_resolution_enabled = false;
    vision::DetectionModule detector(config);
    const cv::Mat dark_frame(240, 320, CV_8UC3, cv::Scalar(0, 0, 0));

    const auto first = detector.Process(dark_frame);
    const auto second = detector.Process(dark_frame);
    Require(!first.box.has_value());
    Require(!second.box.has_value());
    Require(!first.diagnostics.barcode_region_detected);
    Require(!second.diagnostics.used_super_resolution_for_detection);
    Require(!second.diagnostics.used_super_resolution_for_decode);
}
```

In `vision_super_resolution_preview_test.cpp`, extend the existing preview test:

```cpp
vision::VisionProcessingConfig disabled;
disabled.super_resolution_enabled = false;
vision::DetectionModule baseline(disabled);
bool rejected = false;
try {
    static_cast<void>(baseline.SuperResolveForPreview(source));
} catch (const std::logic_error&) {
    rejected = true;
}
Require(rejected);
```

Call both new test functions from their respective `main` functions.

- [ ] **Step 2: Run the characterization tests before refactoring**

Run:

```bash
cmake --build build --target vision_detection_test vision_super_resolution_preview_test --parallel 2
ctest --test-dir build --output-on-failure \
  -R '^(vision_detection_test|vision_super_resolution_preview_test)$'
```

Expected: both tests pass before production code changes, establishing the behavior baseline.

- [ ] **Step 3: Extract barcode-region detection with SR fallback**

Move the initial barcode detection, SR eligibility check, SR execution, retry timing, and inverse corner scaling from
`Process` into `DetectBarcodeRegionsWithFallback`.

The helper must:

```cpp
bool detected = DetectBarcodeRegions(box_roi, corners);
```

record the initial and retry time in `diagnostics.barcode_detection_ms`, set
`used_super_resolution_for_detection` only when the SR attempt is made, return `false` if SR throws through
`TrySuperResolve`, and scale only the retry corners by `1.0F / super_resolution_scale`.

- [ ] **Step 4: Extract decode ROI preparation**

Move the perspective/crop branch into `PrepareBarcodeDecodeRoi`.

For perspective mode, measure `perspective_rectification_ms` and set `used_perspective_rectification` only when the
returned ROI is non-empty. For disabled perspective mode, return `CropBarcode` directly. Do not apply contrast or SR
in this helper.

- [ ] **Step 5: Extract one-candidate decode fallback**

Move contrast enhancement, candidate decode, decoded-barcode append, SR eligibility, SR execution, and SR decode into
`DecodeBarcodeCandidate`.

The helper must preserve this order:

```text
optional contrast enhancement
→ detectAndDecodeWithType
→ append decoded barcode
→ stop when decoded
→ SR only when enabled and ROI size is allowed
→ detectAndDecodeWithType on SR output
→ append decoded barcode using original selected corners
```

Accumulate `contrast_enhancement_ms`, `barcode_decode_ms`, and `super_resolution_ms`; preserve
`used_contrast_enhancement`, `used_super_resolution_for_decode`, and `super_resolution_failed`.

- [ ] **Step 6: Reduce `Process` to orchestration**

Replace the extracted blocks with:

```cpp
const bool detected = DetectBarcodeRegionsWithFallback(
    box_roi, allow_expensive_fallback, reached_failure_threshold,
    detected_corners, result.diagnostics);
```

Retain the existing full-box `DecodeBarcodeRegions` attempt before candidate fallback. In the candidate loop call:

```cpp
cv::Mat decode_roi =
    PrepareBarcodeDecodeRoi(box_roi, detected_corners, barcode_index, result.diagnostics);
if (decode_roi.empty()) {
    continue;
}
DecodeBarcodeCandidate(
    decode_roi, selected_corners, frame_offset, result.barcodes, result.diagnostics);
```

Do not change when `consecutive_barcode_failures_` increments or resets.

- [ ] **Step 7: Build and run vision verification**

Run:

```bash
cmake --build build --target \
  vision_detection_test \
  vision_super_resolution_preview_test \
  logistics_vision_node \
  logistics_vision_benchmark \
  --parallel 2
ctest --test-dir build --output-on-failure \
  -R '^(vision_detection_test|vision_super_resolution_preview_test|vision_benchmark_help|vision_benchmark_visual_output)$'
```

Expected: all four targets build and all four selected tests pass.

- [ ] **Step 8: Format and commit**

Run:

```bash
clang-format -i \
  device-rpi/vision-node/detection.hpp \
  device-rpi/vision-node/detection.cpp \
  device-rpi/tests/vision_detection_test.cpp \
  device-rpi/tests/vision_super_resolution_preview_test.cpp
git diff --check
git add \
  device-rpi/vision-node/detection.hpp \
  device-rpi/vision-node/detection.cpp \
  device-rpi/tests/vision_detection_test.cpp \
  device-rpi/tests/vision_super_resolution_preview_test.cpp
git commit -m "refactor: isolate staged barcode fallbacks"
```

---

### Task 4: Final PR Verification

**Files:**
- Verify only; do not modify production files unless a test exposes a regression.

**Interfaces:**
- Consumes: the three commits from Tasks 1-3.
- Produces: a clean, behavior-preserving PR branch ready for another review.

- [ ] **Step 1: Verify the complete build**

Run:

```bash
cmake -S . -B build \
  -DOpenCV_DIR:PATH="${OPENCV_INSTALL_DIR}/lib/cmake/opencv4"
cmake --build build --parallel 2
```

Expected: configure and build exit with status 0.

- [ ] **Step 2: Run all configured tests**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: zero failed tests.

- [ ] **Step 3: Run formatting and diff checks**

Run:

```bash
FILES=$(find . \
  -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
  -not -path './build/*' \
  -not -path './.git/*' \
  -not -path './control-center/.qtcreator/*')
clang-format --dry-run --Werror $FILES
git diff --check origin/main...HEAD
git status --short
```

Expected: clang-format and diff checks exit with status 0. `git status --short` may show only the intentionally
untracked root `AGENTS.md`; no implementation file may remain modified or untracked.

- [ ] **Step 4: Inspect commit boundaries**

Run:

```bash
git log --oneline -5
git diff --stat origin/main...HEAD
```

Expected: the three refactoring commits appear in order after the design and plan commits, with no protocol,
configuration example, migration, or benchmark-output format changes.
