CREATE TABLE http_upload (
    upload_id TEXT PRIMARY KEY NOT NULL CHECK(length(upload_id) > 0),
    idempotency_key TEXT NOT NULL UNIQUE CHECK(length(idempotency_key) > 0),
    kind TEXT NOT NULL CHECK(kind IN ('IMAGE','LOG')),
    device_id TEXT NOT NULL CHECK(length(device_id) > 0),
    work_id TEXT REFERENCES product(work_id) ON DELETE RESTRICT,
    relative_path TEXT NOT NULL UNIQUE CHECK(length(relative_path) > 0),
    mime_type TEXT NOT NULL CHECK(length(mime_type) > 0),
    byte_size INTEGER NOT NULL CHECK(byte_size > 0),
    sha256 TEXT NOT NULL CHECK(length(sha256) = 64),
    captured_at TEXT,
    started_at TEXT,
    ended_at TEXT,
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0)
);

CREATE INDEX idx_http_upload_work ON http_upload(work_id, created_at_ms) WHERE work_id IS NOT NULL;
CREATE INDEX idx_http_upload_device ON http_upload(device_id, created_at_ms);
CREATE INDEX idx_http_upload_created ON http_upload(created_at_ms);
