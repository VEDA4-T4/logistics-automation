# Device common runtime

`device_runtime` contains shared Raspberry Pi node components. The log spool uploader rotates logs by size or time,
persists pending files across process restarts, and uploads them to the central server with multipart HTTP(S). The
vision node also uses the common image uploader before publishing `PRODUCT_IMAGE` metadata over MQTT.

Required Raspberry Pi packages:

```sh
sudo apt-get install libcurl4-openssl-dev libssl-dev pkg-config
```

Configure `[log_upload]` in `config/node.ini`; `NodeRuntime` starts and stops `LogSpoolUploader` with the MQTT client.
Upload work runs on a background thread. A pending file is removed only after the central server returns HTTP 200/201
with the same SHA-256 checksum.

Configure `[image_upload]` for the vision node. Image bytes are sent with HTTP(S), not MQTT. The node publishes the
server-returned image ID, path, and checksum only after it validates a successful HTTP 200/201 response.

Plain HTTP is rejected unless `allow_insecure_http` is explicitly enabled for local integration testing.
