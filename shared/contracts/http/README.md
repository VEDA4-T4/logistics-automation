# HTTP(S) image and log upload contract

Binary images and rotated log bundles are transferred with HTTP(S). MQTT carries
real-time events and the metadata returned after a successful upload; file bytes
must never be embedded in an MQTT payload.

## Endpoints

- `POST /api/v1/uploads/images`
- `POST /api/v1/uploads/logs`

Production deployments use HTTPS. A server may allow HTTP only for an explicitly
configured local integration environment. Devices authenticate with
`Authorization: Bearer <device-token>`.

Both endpoints accept `multipart/form-data` with one `file` part. All textual
metadata uses UTF-8.

Common fields:

- `deviceId`
- `messageId`
- `sha256`: 64 lowercase hexadecimal characters
- `byteSize`
- HTTP header `Idempotency-Key`: identical to `messageId`

Image-only fields:

- `workId`
- `capturedAt`: ISO 8601 timestamp with UTC `Z` or an explicit offset
- MIME type `image/jpeg` or `image/png`
- maximum size 10 MiB

Log-only fields:

- `startedAt` and `endedAt`: ISO 8601 timestamps
- MIME type `text/plain`, `application/gzip`, or `application/zip`
- maximum size 25 MiB

## Success response

New uploads return HTTP 201. A repeated idempotency key with identical metadata
returns HTTP 200 and the original resource. The JSON response is:

```json
{
  "uploadId": "server-generated-id",
  "path": "/uploads/images/server-generated-id",
  "checksum": "64-lowercase-hex-characters",
  "duplicate": false
}
```

The server rejects reuse of an idempotency key with different metadata.

## Errors and retry policy

- 400: missing or malformed metadata
- 401/403: device authentication failure
- 404: referenced work does not exist
- 409: idempotency key conflicts with different metadata
- 413: file exceeds the endpoint limit
- 415: unsupported MIME type
- 422: byte size or SHA-256 mismatch
- 500/503: storage or database failure

Clients retry connection failures, 408, 429, and 5xx responses with exponential
backoff. Other 4xx responses require operator or configuration correction. A
spooled file is deleted only after a 200 or 201 response whose checksum matches
the local file.
