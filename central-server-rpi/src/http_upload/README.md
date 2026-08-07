# HTTP upload server

The central server accepts image and device-log multipart uploads defined in
`shared/contracts/http/README.md`. It validates the bearer token, idempotency key, metadata, size, MIME type, SHA-256,
and image `workId` before committing the file and SQLite record.

Required Raspberry Pi packages:

```sh
sudo apt-get install libmicrohttpd-dev libssl-dev libsqlite3-dev pkg-config
```

Copy `central-server-rpi/config/server.ini.example`, replace `bearer_token`, and enable TLS for production. The HTTP
listener starts and stops with `logistics_central_server`. It listens on the configured port for connections from other
machines, so allow that port through the server host firewall.

The vision node must use the same token and an address reachable from its Raspberry Pi:

```ini
# central-server-rpi server.ini
[http]
enabled=true
port=8080
bearer_token=replace-with-a-device-upload-token

# device-rpi vision-node.ini
[image_upload]
enabled=true
endpoint_url=http://192.168.0.10:8080/api/v1/uploads/images
bearer_token=replace-with-a-device-upload-token
allow_insecure_http=true
```

Use the central server machine's LAN IP instead of `127.0.0.1` when the vision node runs on a different device. Plain
HTTP is intended only for an isolated integration network; configure `tls_enabled=true` and an HTTPS endpoint for
deployment.

## History queries

The same HTTP listener exposes read-only global, work, and device history endpoints:

```text
GET /api/v1/history?limit=100&cursor={nextCursor}
GET /api/v1/history/work/{workId}?limit=100&cursor={nextCursor}
GET /api/v1/history/device/{deviceId}?limit=100&cursor={nextCursor}
Authorization: Bearer <http.bearer_token>
```

`limit` defaults to 100 and must be between 1 and 500. Results are returned newest first. Pass a non-null `nextCursor`
back unchanged to load the next page. The cursor shares the query's timestamp and row-identity ordering, so records
with equal timestamps are paged without gaps or repeated rows. Work history combines process transitions and recorded
errors; global and device history combine retained MQTT events and device errors. MQTT events, device status, errors,
security logs, and images use the retention periods configured in the `[storage]` section.
