CREATE TABLE process_command_outbox (
    request_id TEXT PRIMARY KEY NOT NULL CHECK(length(request_id) > 0),
    message_json TEXT NOT NULL CHECK(length(message_json) > 0),
    work_id TEXT NOT NULL CHECK(length(work_id) > 0),
    dispatched_event TEXT,
    dispatch_confirmed INTEGER NOT NULL CHECK(dispatch_confirmed IN (0, 1)),
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0)
);
