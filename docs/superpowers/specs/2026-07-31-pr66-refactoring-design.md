# PR 66 Refactoring Design

## Purpose

Refactor only the code introduced or materially expanded by PR 66. Preserve the MQTT protocol, INI configuration,
database schema, runtime behavior, and benchmark output while making the restart-recovery and staged barcode
processing flows easier to understand and test.

## Scope

The refactoring covers:

- central-server restoration of homography-derived gripper targets;
- persistence and Qt notification of work invalidated by calibration changes;
- vision-node barcode detection, rectification, decoding, and super-resolution fallback orchestration.

The refactoring does not cover:

- splitting the complete central-server `application.cpp`;
- introducing a general startup recovery framework;
- changing MQTT message fields or topics;
- changing homography calculations or calibration semantics;
- changing SR algorithms, thresholds, model formats, benchmark CSV columns, or failure-frame retention.

## Design Principles

- Prefer explicit return values over result data retained as mutable object state.
- Keep calibration-specific behavior visible instead of introducing a generic recovery framework with one caller.
- Extract only independently testable stages that already exist in the processing flow.
- Preserve existing public configuration and protocol contracts.
- Keep each refactoring commit behavior-preserving and independently testable.

## Central-Server Restore Result

`ProcessOrchestrator::RestoreAfterServerRestart` currently returns `bool` and exposes invalidated work through
`InvalidatedRestoredWorks()`. Replace this side channel with a `ProcessRestoreResult` value containing:

- whether the stored state was valid and restored;
- the list of `InvalidatedRestoredWork` records;
- each invalidated work ID and its operator-facing reason.

The orchestrator will no longer retain invalidation records after restoration. Gripper targets that reference missing
work remain silently discarded because they are orphaned state, while targets using a stale calibration invalidate
their corresponding work and appear in the result.

The application consumes the result immediately:

1. reject startup if restoration failed;
2. persist each calibration invalidation;
3. save the filtered process snapshot;
4. after MQTT connects, publish one Qt error for each invalidation.

## Work Invalidation Persistence and Notification

Replace the positional `InvalidateWork(work_id, error_code, message, occurred_at_ms)` call with a
`WorkInvalidation` value. It contains:

- `work_id`;
- `message_id`;
- `error_code`;
- `reason`;
- `cause`;
- `occurred_at_ms`.

`PersistenceService::RecordWorkInvalidation` atomically:

- changes the product lifecycle to `ERROR`;
- appends one `ERROR_OCCURRED` work-history record;
- appends one error-log record.

The operation remains idempotent by `message_id`. A retry updates the product lifecycle but does not duplicate history
or error records.

A pure central-server helper creates the corresponding MQTT `ERROR_OCCURRED` message from the same invalidation
value. The application owns retry timing: notification remains pending until the MQTT client is connected and publish
succeeds. This preserves the current behavior without creating a new coordinator class.

## Vision Staged Barcode Processing

Keep `DetectionModule` as the owner of OpenCV objects and consecutive-failure state. Split the existing orchestration
inside `Detect` into private helpers with narrow responsibilities:

- detect barcode candidates, optionally using the existing SR detection fallback;
- build a decode ROI using perspective rectification or the existing padded crop fallback;
- decode one candidate through normal, contrast-enhanced, and SR attempts.

The helpers return existing result and diagnostics data rather than adding another processing class. Timing fields,
fallback flags, failure thresholds, maximum input-pixel checks, corner mapping, and exception-to-diagnostic behavior
remain unchanged.

`Detect` remains responsible for:

- detecting the box;
- updating the consecutive barcode-failure counter;
- sequencing the helper calls;
- resetting the counter after successful barcode detection;
- returning the final `DetectionResult`.

## Error Handling

- Invalid persisted process state remains a fatal startup error.
- A stale calibration is a per-work invalidation, not a server-wide process error.
- Failure to persist an invalidation is logged and does not restore unsafe coordinates.
- Failure to publish a Qt notification remains retryable while MQTT is connected.
- OpenCV SR failures remain represented by `super_resolution_failed`; they do not terminate the vision process.
- Empty or invalid barcode ROIs continue to skip that candidate without throwing.

## Testing

Central-server tests will verify:

- restore results contain stale-calibration invalidations directly;
- the orchestrator retains no invalidation side-channel state;
- the old work is absent and a replacement work can accept fresh coordinates;
- work invalidation persistence is atomic and idempotent;
- MQTT recalibration errors contain the expected work ID, code, state, level, and reason.

Vision verification will use the existing detection, SR preview, benchmark-help, and benchmark visual-output tests as
the behavior baseline. The refactoring does not add a generated barcode fixture, expose private processing state, or
introduce a detector-injection seam solely for tests. Review of the extracted code must confirm that fallback order,
threshold checks, selected ROI use, diagnostics, and timing accumulation are moved without semantic changes.

Each refactoring commit must pass the directly affected tests, clang-format, and compilation of the changed production
sources. The final branch verification runs all central-server and vision tests available in the CI configuration.

## Commit Boundaries

1. `refactor: return process restore results explicitly`
2. `refactor: encapsulate work invalidation reporting`
3. `refactor: isolate staged barcode fallbacks`

No commit changes protocol, configuration, persistence schema, or user-visible processing behavior.
