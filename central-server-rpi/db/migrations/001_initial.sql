CREATE TABLE product (
    work_id TEXT PRIMARY KEY NOT NULL CHECK(length(work_id) > 0),
    barcode TEXT,
    product_name TEXT,
    destination TEXT,
    lifecycle_state TEXT NOT NULL CHECK(lifecycle_state IN
        ('DETECTED','IMAGED','IDENTIFIED','DESTINATION_SET','COMPLETED','ERROR')),
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= created_at_ms),
    completed_at_ms INTEGER CHECK(completed_at_ms IS NULL OR completed_at_ms >= created_at_ms)
);

CREATE INDEX idx_product_barcode ON product(barcode) WHERE barcode IS NOT NULL;
CREATE INDEX idx_product_updated ON product(updated_at_ms);

CREATE TABLE work_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    work_id TEXT NOT NULL REFERENCES product(work_id) ON DELETE RESTRICT,
    message_id TEXT NOT NULL UNIQUE CHECK(length(message_id) > 0),
    event_type TEXT NOT NULL CHECK(length(event_type) > 0),
    process_state TEXT,
    source_id TEXT NOT NULL CHECK(length(source_id) > 0),
    details_json TEXT NOT NULL DEFAULT '{}',
    occurred_at_ms INTEGER NOT NULL CHECK(occurred_at_ms >= 0)
);

CREATE INDEX idx_work_history_work_time ON work_history(work_id, occurred_at_ms);

CREATE TABLE image_file (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    work_id TEXT NOT NULL REFERENCES product(work_id) ON DELETE RESTRICT,
    message_id TEXT NOT NULL UNIQUE CHECK(length(message_id) > 0),
    relative_path TEXT NOT NULL UNIQUE CHECK(length(relative_path) > 0),
    mime_type TEXT NOT NULL CHECK(mime_type IN ('image/jpeg','image/png','image/webp')),
    byte_size INTEGER NOT NULL CHECK(byte_size >= 0),
    sha256 TEXT NOT NULL CHECK(length(sha256) = 64),
    captured_at_ms INTEGER NOT NULL CHECK(captured_at_ms >= 0),
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0)
);

CREATE INDEX idx_image_work ON image_file(work_id, captured_at_ms);
CREATE INDEX idx_image_created ON image_file(created_at_ms);

CREATE TABLE device_status (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL CHECK(length(device_id) > 0),
    message_id TEXT NOT NULL UNIQUE CHECK(length(message_id) > 0),
    role TEXT CHECK(role IS NULL OR role IN ('input','vision','sorting','linetracer')),
    connection_state TEXT NOT NULL CHECK(connection_state IN
        ('ONLINE','DELAYED','OFFLINE','RECONNECTING','RTSP_ERROR','MQTT_ERROR',
         'MQTT_AUTH_ERROR','TLS_ERROR','UART_ERROR','UNKNOWN')),
    process_state TEXT CHECK(process_state IS NULL OR process_state IN
        ('IDLE','RUNNING','STOPPED','ERROR','ESTOP','RECOVERY','UNKNOWN')),
    status_json TEXT NOT NULL DEFAULT '{}',
    observed_at_ms INTEGER NOT NULL CHECK(observed_at_ms >= 0)
);

CREATE INDEX idx_device_status_latest ON device_status(device_id, observed_at_ms DESC);
CREATE INDEX idx_device_status_time ON device_status(observed_at_ms);

CREATE TABLE error_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    message_id TEXT UNIQUE,
    work_id TEXT REFERENCES product(work_id) ON DELETE RESTRICT,
    device_id TEXT NOT NULL CHECK(length(device_id) > 0),
    component_id TEXT,
    error_code TEXT NOT NULL CHECK(length(error_code) > 0),
    severity TEXT NOT NULL CHECK(severity IN ('INFO','WARNING','ERROR','CRITICAL')),
    error_message TEXT NOT NULL,
    details_json TEXT NOT NULL DEFAULT '{}',
    occurred_at_ms INTEGER NOT NULL CHECK(occurred_at_ms >= 0),
    resolved_at_ms INTEGER CHECK(resolved_at_ms IS NULL OR resolved_at_ms >= occurred_at_ms)
);

CREATE INDEX idx_error_time ON error_log(occurred_at_ms);
CREATE INDEX idx_error_work ON error_log(work_id) WHERE work_id IS NOT NULL;

CREATE TABLE mqtt_event_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    message_id TEXT,
    topic TEXT NOT NULL CHECK(length(topic) > 0),
    protocol_version TEXT,
    message_type TEXT,
    source_id TEXT,
    payload_json TEXT NOT NULL,
    qos INTEGER NOT NULL CHECK(qos IN (0, 1)),
    retained INTEGER NOT NULL CHECK(retained IN (0, 1)),
    processing_state TEXT NOT NULL CHECK(processing_state IN ('RECEIVED','STORED','REJECTED')),
    failure_reason TEXT,
    received_at_ms INTEGER NOT NULL CHECK(received_at_ms >= 0),
    processed_at_ms INTEGER,
    duplicate_count INTEGER NOT NULL DEFAULT 0 CHECK(duplicate_count >= 0),
    last_received_at_ms INTEGER NOT NULL CHECK(last_received_at_ms >= received_at_ms)
);

CREATE UNIQUE INDEX uq_mqtt_message_id ON mqtt_event_log(message_id) WHERE message_id IS NOT NULL;
CREATE INDEX idx_mqtt_received ON mqtt_event_log(received_at_ms);

CREATE TABLE security_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_type TEXT NOT NULL CHECK(length(event_type) > 0),
    actor_id TEXT,
    source_address TEXT,
    outcome TEXT NOT NULL CHECK(outcome IN ('ALLOWED','DENIED','INVALID')),
    details_json TEXT NOT NULL DEFAULT '{}',
    occurred_at_ms INTEGER NOT NULL CHECK(occurred_at_ms >= 0)
);

CREATE INDEX idx_security_time ON security_log(occurred_at_ms);
