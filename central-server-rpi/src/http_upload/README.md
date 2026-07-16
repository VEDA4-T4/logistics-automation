# HTTP upload server

The central server accepts image and device-log multipart uploads defined in
`shared/contracts/http/README.md`. It validates the bearer token, idempotency key, metadata, size, MIME type, SHA-256,
and image `workId` before committing the file and SQLite record.

Required Raspberry Pi packages:

```sh
sudo apt-get install libmicrohttpd-dev libssl-dev libsqlite3-dev pkg-config
```

Copy `central-server-rpi/config/server.ini.example`, replace `bearer_token`, and enable TLS for production. The
`--once` option performs migration and retention checks without starting the HTTP listener.
