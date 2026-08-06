CREATE TABLE device_status_v3 (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL CHECK(length(device_id) > 0),
    message_id TEXT NOT NULL UNIQUE CHECK(length(message_id) > 0),
    role TEXT CHECK(role IS NULL OR role IN ('input','vision','sorting','linetracer')),
    connection_state TEXT NOT NULL CHECK(connection_state IN
        ('ONLINE','DELAYED','OFFLINE','RECONNECTING','RTSP_ERROR','MQTT_ERROR',
         'MQTT_AUTH_ERROR','TLS_ERROR','UART_ERROR','UNKNOWN')),
    process_state TEXT CHECK(process_state IS NULL OR
        (length(process_state) > 0 AND length(process_state) <= 128)),
    status_json TEXT NOT NULL DEFAULT '{}',
    observed_at_ms INTEGER NOT NULL CHECK(observed_at_ms >= 0)
);

INSERT INTO device_status_v3(
    id,device_id,message_id,role,connection_state,process_state,status_json,observed_at_ms
)
SELECT id,device_id,message_id,role,connection_state,process_state,status_json,observed_at_ms
FROM device_status;

DROP TABLE device_status;
ALTER TABLE device_status_v3 RENAME TO device_status;

CREATE INDEX idx_device_status_latest ON device_status(device_id, observed_at_ms DESC);
CREATE INDEX idx_device_status_time ON device_status(observed_at_ms);
