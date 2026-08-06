CREATE TABLE process_runtime_state (
    id INTEGER PRIMARY KEY NOT NULL CHECK(id = 1),
    system_state TEXT NOT NULL CHECK(system_state IN
        ('IDLE','RUNNING','STOPPED','ERROR','ESTOP','RECOVERY')),
    message_sequence INTEGER NOT NULL CHECK(message_sequence >= 0),
    updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= 0)
);

CREATE TABLE process_work_state (
    work_id TEXT PRIMARY KEY NOT NULL CHECK(length(work_id) > 0),
    stage TEXT NOT NULL CHECK(length(stage) > 0),
    suspended_stage TEXT,
    destination TEXT NOT NULL DEFAULT '',
    last_source_id TEXT NOT NULL DEFAULT '',
    failure_reason TEXT NOT NULL DEFAULT '',
    updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= 0)
);
