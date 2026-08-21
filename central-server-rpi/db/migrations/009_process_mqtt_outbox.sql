CREATE TABLE process_mqtt_outbox (
    topic TEXT NOT NULL CHECK(length(topic) > 0),
    message_id TEXT NOT NULL CHECK(length(message_id) > 0),
    message_json TEXT NOT NULL CHECK(length(message_json) > 0),
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
    PRIMARY KEY(topic, message_id)
);

CREATE TABLE process_processed_message (
    message_id TEXT PRIMARY KEY CHECK(length(message_id) > 0),
    sequence INTEGER NOT NULL CHECK(sequence >= 0)
);

CREATE TABLE command_manager_pending (
    request_id TEXT PRIMARY KEY,
    message_json TEXT NOT NULL,
    expected_devices_json TEXT NOT NULL,
    completed_devices_json TEXT NOT NULL,
    response_message_ids_json TEXT NOT NULL,
    failure_json TEXT,
    deadline_at_ms INTEGER NOT NULL
);

CREATE TABLE command_manager_completed (
    request_id TEXT PRIMARY KEY,
    sequence INTEGER NOT NULL
);

CREATE TABLE command_manager_runtime (
    id INTEGER PRIMARY KEY CHECK(id=1),
    message_sequence INTEGER NOT NULL
);

CREATE TABLE pending_system_command (
    request_id TEXT PRIMARY KEY,
    command TEXT NOT NULL
);
