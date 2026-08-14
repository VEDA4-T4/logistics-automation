CREATE INDEX idx_mqtt_pending_received
ON mqtt_event_log(received_at_ms, id)
WHERE processing_state = 'RECEIVED';
