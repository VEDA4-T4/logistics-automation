# Central server and vision node setup scripts

Run these scripts from a checkout of `feature/vision-node-integrate`. Runtime configuration and generated data are
written below the repository's ignored `runtime/` directory. Existing INI files are preserved unless
`LOGISTICS_FORCE_CONFIG=1` is set.

## 1. Central server

```sh
export LOGISTICS_UPLOAD_TOKEN='replace-with-a-long-random-token'
export LOGISTICS_MQTT_HOST='127.0.0.1'
./deploy/scripts/setup-central-server.sh
```

The script creates `runtime/central-server/server.ini` and its storage directories, builds the central server, and runs
its tests. It does not modify, start, or restart Mosquitto. Existing broker configuration remains untouched.

## 2. Vision Raspberry Pi

Use the same upload token and the central server's LAN address:

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_UPLOAD_TOKEN='replace-with-a-long-random-token'
export LOGISTICS_DEVICE_ID='PI-VISION-01'
export LOGISTICS_DEVICE_IP='192.168.0.21'
./deploy/scripts/setup-vision-node.sh
```

OpenCV 4.10.0 is required. To let the script build it from source when it is absent:

```sh
export LOGISTICS_INSTALL_OPENCV=1
./deploy/scripts/setup-vision-node.sh
```

The generated runtime configuration is `runtime/vision-node/vision-node.ini`.

The setup scripts assume build dependencies are already installed. To install the required Ubuntu packages as part of
the run, explicitly opt in:

```sh
export LOGISTICS_INSTALL_DEPENDENCIES=1
```

## 3. Connectivity check

Run this from the Vision Pi after the MQTT broker and central server have started:

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
./deploy/scripts/check-connectivity.sh
```

Optional variables include `LOGISTICS_MQTT_HOST`, `LOGISTICS_CONFIG_PATH`, `LOGISTICS_BUILD_DIR`,
`LOGISTICS_RUNTIME_DIR`, `LOGISTICS_NODE_NAME`, and `LOGISTICS_FORCE_CONFIG=1`. Do not place tokens or passwords directly
in these scripts.
