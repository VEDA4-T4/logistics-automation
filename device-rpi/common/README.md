# Device common runtime

`device_runtime` contains shared Raspberry Pi node components. The log spool uploader rotates logs by size or time,
persists pending files across process restarts, and uploads them to the central server with multipart HTTP(S).

Required Raspberry Pi packages:

```sh
sudo apt-get install libcurl4-openssl-dev libssl-dev pkg-config
```

Configure `[log_upload]` in `config/node.ini`; `NodeRuntime` starts and stops `LogSpoolUploader` with the MQTT client.
Upload work runs on a background thread. A pending file is removed only after the central server returns HTTP 200/201
with the same SHA-256 checksum.

Plain HTTP is rejected unless `allow_insecure_http` is explicitly enabled for local integration testing.
