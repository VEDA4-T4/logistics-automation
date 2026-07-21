# Vision node MQTT setup

The vision node connects to the same MQTT broker as the central server and control center. The processes may run on
different machines; set `host` and the HTTP upload URL to addresses reachable from the vision Raspberry Pi. Do not use
`127.0.0.1` unless the broker or server is running on that same Raspberry Pi.

## Raspberry Pi dependencies

```sh
sudo apt update
sudo apt install libmosquitto-dev libcurl4-openssl-dev libssl-dev nlohmann-json3-dev pkg-config
```

OpenCV 4.10 with the `core`, `highgui`, `imgcodecs`, `imgproc`, `objdetect`, and `videoio` components must also be
available to CMake.

## Configuration

Copy `device-rpi/config/node.ini.example` outside the source tree and change at least these values:

```ini
[device]
device_id=PI-VISION-01
node_name=vision-node-01
ip_address=192.168.0.21

[mqtt]
host=192.168.0.10
port=1883
client_id=PI-VISION-01

[image_upload]
enabled=true
endpoint_url=http://192.168.0.10:8080/api/v1/uploads/images
bearer_token=replace-with-the-central-server-device-token
allow_insecure_http=true
```

Use HTTPS in deployment. Plain HTTP is accepted only when `allow_insecure_http=true` is explicitly configured for an
integration network.

## Run

```sh
./logistics_vision_node --config runtime/vision-node/vision-node.ini
```

The runtime sequence is:

1. The vision node connects, publishes registration, and starts heartbeats.
2. After a box is stable for the configured confirmation frames, it publishes `BOX_DETECTED`.
3. The central server creates a work and sends `WORK_CREATED` to the vision node.
4. The vision node publishes `POSITION_DETECTED` and `BARCODE_DETECTED` for that work.
5. If image upload is enabled, it uploads the JPEG over HTTP(S), verifies the response, and publishes `PRODUCT_IMAGE`.
6. Camera, encoding, upload, and MQTT publication failures are reported with `ERROR_OCCURRED`.

The central server must therefore be running with its MQTT subscriptions and image upload endpoint enabled before the
complete scenario can finish. The vision node will not invent a work ID locally; it waits for `WORK_CREATED` so all
subsequent events use the server-assigned work ID.
