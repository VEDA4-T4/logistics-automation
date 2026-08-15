# Task 6 report: isolate process messages by epoch

## Outcome

- Added backward-compatible optional MQTT envelope `processEpoch`; absent legacy envelopes still decode.
- Added the shared `IsProcessScopedMessage` classification and UUID validation.
- Fresh startup generates and persists a UUID epoch; resume restores the stored epoch. Ordinary STOP and RECOVERY do not replace it.
- Central stamps system/process commands, WORK_CREATED, synthetic BOX_DETECTED, catalog/recovery outputs, and other durable work-scoped output while leaving raw SENSOR_STATUS and current telemetry epoch-free.
- Common node MQTT processing owns the active epoch. START, RESTART, and INITIALIZE replace it; WORK_CREATED establishes it for a late node and rejects conflicts; STOP and RECOVERY retain it. Work-scoped responses/events/statuses inherit it.
- Central rejects stale work-scoped messages before process guards, handlers, derived persistence, or routing. Fresh mode also rejects missing epochs. Rejections are durably terminal as `REJECTED_STALE_EPOCH`, so inbox replay is not blocked.
- Input work creation is armed only after a CLEAR observation and is re-armed on START, RESTART, STOP, and RECOVERY. Raw sensor telemetry remains routable.
- Added migration `011_process_epoch.sql` and runtime snapshot round-trip coverage.

## Exact process-scoped classification

Always work-scoped:

- `BOX_DETECTED`
- `WORK_CREATED`
- `WORK_COMPLETED`
- `POSITION_DETECTED`
- `BARCODE_DETECTED`
- `PRODUCT_IMAGE`
- `PRODUCT_INFO`
- `DESTINATION_SET`

Conditionally work-scoped:

- `DEVICE_STATUS` when `jobId` is present
- `ERROR_OCCURRED` when `jobId` is present
- `CONTROL_COMMAND` when `params.workId` is present
- `COMMAND_RESPONSE` except `STATUS_REQUEST` responses

Explicitly current telemetry/non-work-scoped:

- `DEVICE_REGISTER`
- `HEARTBEAT`
- `SENSOR_STATUS`
- `DEVICE_STATUS` without `jobId`
- `ERROR_OCCURRED` without `jobId`
- `STATUS_REQUEST` responses
- `EMERGENCY_STOP` is not classified as a work event, but central stamps it as a process-control boundary.

## Ownership

- Central owns the process epoch lifecycle in `Application` and persists it through `ProcessStateStore`.
- `MqttHandler` owns ingress comparison and terminal stale rejection.
- Each node's `MqttMessageProcessor` owns the node-local active epoch and request correlation needed for late first commands.

## TDD evidence

Expected RED failures observed before production changes:

- shared contract test: missing `MqttMessage::process_epoch` and `IsProcessScopedMessage`
- node processor test: missing outbound epoch preparation/propagation
- node late-start test: WORK_CREATED did not establish or reject conflicting epochs
- sensor gate test: missing CLEAR arming/`RequireClear`
- state-store test: missing epoch generation, storage, and load
- MQTT handler test: missing central epoch configuration and stale terminal rejection
- recovery integration test: recovery response and generated failure completion omitted the epoch
- node self-review test: STOP response preferred the incoming request epoch over the active epoch
- node duplicate self-review test: rejected duplicate START could mutate the active epoch before messageId validation
- full storage test: migration fixtures still assumed ten migrations/a four-column runtime row

All corresponding GREEN tests passed after minimal implementation.

## Verification

- Focused Task 6 targets built and passed: contracts, node MQTT processor, sensor detection, process state store, MQTT handler, storage, and process integration.
- `cmake --build build-process-recovery -j2`: passed with vision/control-center disabled in the existing OpenCV-free configuration.
- `ctest --test-dir build-process-recovery --output-on-failure`: 64/64 passed.
- After adding explicit `{}` defaults to avoid new aggregate-initializer warning noise, focused single-job builds and tests for contracts, node MQTT processing, and central storage passed again.
- LLVM `clang-format --dry-run --Werror --style=file` on every modified C++ file: passed.
- `git diff --check`: passed (Git emitted only the repository's LF/CRLF conversion warning for `application_recovery.cpp`).
- No OpenCV installation/use, SSH, STM firmware change, push, or hardware deployment was performed.

## Additional necessary files beyond the plan list

- `central-server-rpi/db/migrations/011_process_epoch.sql`: schema persistence.
- `central-server-rpi/include/logistics/central_server/process_state_store.hpp`: snapshot/API contract.
- `central-server-rpi/include/logistics/central_server/mqtt_handler.hpp`: central epoch configuration state.
- `central-server-rpi/include/logistics/central_server/persistence.hpp` and `src/db_manager/persistence.cpp`: atomic terminal inbox rejection without derived process persistence.
- `central-server-rpi/include/logistics/central_server/sensor_detection.hpp`, `src/mqtt_handler/sensor_detection.cpp`, and tests: explicit CLEAR arming.
- `central-server-rpi/src/application_recovery.cpp` and `tests/process_integration_test.cpp`: recovery-generated work response/event propagation.
- `central-server-rpi/tests/process_state_store_test.cpp` and `tests/storage_test.cpp`: DB restart/fresh/migration coverage.
- `device-rpi/common/include/logistics/device/mqtt_message_processor.hpp`: node-local epoch state.

## Self-review

- Stale checks precede process guards, derived persistence, and routing.
- Rejected rows are terminal and excluded from `PendingReceivedEvents` replay.
- Active node epoch changes only after topic/envelope/messageId validation; conflicting WORK_CREATED and reused message IDs cannot poison it.
- Active epoch takes precedence for work-scoped responses, so STOP/RECOVERY cannot change response epoch; request correlation is only a fallback when a late node has no active epoch.
- Heartbeat, non-work device status/error, and SENSOR_STATUS remain usable without an epoch.
- No second session/state framework or new dependency was introduced.
- The optional envelope members have explicit default member initializers, so legacy aggregate construction does not add `process_epoch` missing-field warnings.

## Fix round 1

Reviewer findings were reproduced with focused RED tests and corrected without changing the process-epoch lifecycle:

- Qt `DEVICE_STATUS` reconstruction now preserves an inbound status epoch. Heartbeat timeout and snapshot statuses with a `jobId` use central's current epoch; heartbeat and no-job status telemetry remain epoch-free.
- `CommitRecoveryResponse` now receives the persisted current epoch explicitly. The Qt recovery response and every recovery-generated `WORK_COMPLETED` use it even when the resumed device response is legacy and has no epoch.
- One testable `Application::StampProcessEpoch` seam is used by new durable delivery, restored pending-command, and durable replay paths. Legacy process rows are stamped in memory immediately before publish without changing topic/message ID; matching epochs are preserved; telemetry is unchanged; conflicting restored rows are removed instead of overwritten.
- Correlated `COMMAND_RESPONSE` preparation now prefers the request epoch and falls back to the active epoch. A delayed E1 work response remains E1 after START establishes E2, while the node's active epoch remains E2. Explicitly conflicting prepared epochs are still rejected.

Focused RED evidence:

- `central_server_mqtt_handler_test` failed at the inbound job-status epoch assertion.
- `central_server_process_integration_test` failed to compile because the recovery seam lacked a persisted-epoch argument and durable replay had no testable stamping seam.
- `device_mqtt_message_processor_test` failed because the STOP response used active E1 instead of its correlated request E2.

Focused GREEN verification after formatting:

- `cmake --build build-process-recovery --target logistics_central_server central_server_mqtt_handler_test central_server_process_integration_test device_mqtt_message_processor_test -j1`: passed.
- Sequential CTest: `central_server_mqtt_handler_test` 1/1, `central_server_process_integration_test` 1/1, and `device_mqtt_message_processor_test` 1/1 passed.
- LLVM `clang-format` applied to all eight modified C++ files; `git diff --check` passed.
- Direct Qt status and durable replay/publish call sites were reviewed. Existing direct WORK_CREATED/recovery outputs already carry the current epoch; raw telemetry remains exempt.
- Per resource policy, no full local build or full CTest was run in this fix round. The prior 64/64 Task 6 run remains the full-suite baseline. No SSH, push, OpenCV, or STM work was performed.

This round supersedes the earlier self-review statement that active epoch takes precedence for work-scoped responses: request-correlated command responses intentionally use their request epoch, while uncorrelated work-scoped output uses the active epoch.
