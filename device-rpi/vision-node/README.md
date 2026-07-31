# Vision node MQTT setup

The vision node connects to the same MQTT broker as the central server and control center. The processes may run on
different machines; set `host` and the HTTP upload URL to addresses reachable from the vision Raspberry Pi. Do not use
`127.0.0.1` unless the broker or server is running on that same Raspberry Pi.

## Raspberry Pi dependencies

```sh
sudo apt update
sudo apt install libmosquitto-dev libcurl4-openssl-dev libssl-dev nlohmann-json3-dev pkg-config
```

OpenCV 4.10 with the `core`, `dnn`, `highgui`, `imgcodecs`, `imgproc`, `objdetect`, and `videoio` components must also be
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

[vision_processing]
perspective_rectification=true
contrast_enhancement=true
super_resolution_enabled=true
barcode_detection_fallback=true
barcode_decode_fallback=true
super_resolution_backend=bicubic
super_resolution_scale=2
failure_frames_before_super_resolution=2
maximum_super_resolution_input_pixels=300000
```

Use HTTPS in deployment. Plain HTTP is accepted only when `allow_insecure_http=true` is explicitly configured for an
integration network.

## Run

```sh
./logistics_vision_node --headless --config runtime/vision-node/vision-node.ini
```

`--headless` disables the OpenCV preview window for SSH and systemd operation. Without this option, the node also
switches to headless mode automatically when neither `DISPLAY` nor `WAYLAND_DISPLAY` is available.

The runtime sequence is:

1. The vision node connects, publishes registration, and starts heartbeats.
2. After a box is stable for the configured confirmation frames, it publishes `BOX_DETECTED`.
3. The central server creates a work and sends `WORK_CREATED` to the vision node.
4. The vision node reads the barcode attached to the product's top face and publishes `POSITION_DETECTED` and
   `BARCODE_DETECTED` for that work.
5. If image upload is enabled, it uploads the JPEG over HTTP(S), verifies the response, and publishes `PRODUCT_IMAGE`.
6. Camera, encoding, upload, and MQTT publication failures are reported with `ERROR_OCCURRED`.

The central server must therefore be running with its MQTT subscriptions and image upload endpoint enabled before the
complete scenario can finish. The vision node will not invent a work ID locally; it waits for `WORK_CREATED` so all
subsequent events use the server-assigned work ID.

The product orientation is a system input constraint: the barcode must face upward when the product enters the vision
area. The vision node does not request product rotation or search the other five faces.

## Barcode fallback pipeline

The normal path detects the box, detects barcode regions inside the original box ROI, and decodes EAN-13 from the
original pixels. Expensive processing is not run continuously. Once a box has been announced and its barcode is still
missing, the node waits for `failure_frames_before_super_resolution` consecutive failures and then:

1. retries barcode-region detection on a 2x super-resolved box ROI;
2. maps every detected point back to the original frame coordinate system;
3. perspective-rectifies only the detected barcode quadrilateral;
4. applies CLAHE and mild unsharp masking;
5. retries decoding, and finally retries once more on the super-resolved barcode ROI.

`bicubic` works without an external model and provides the deployment baseline. For learned SR, set
`super_resolution_backend=fsrcnn` and `super_resolution_model_path` to a TensorFlow `.pb` model whose scale matches
`super_resolution_scale`. Model files are deployment assets and are not stored in this repository.

## Vision ablation benchmark

Keep representative camera frames in a private dataset directory and create a CSV manifest:

```csv
filename,ean13
clear-01.jpg,8801234567893
blurred-01.jpg,8801234567893
angled-01.jpg,8801234567893
```

[`benchmark/manifest.example.csv`](benchmark/manifest.example.csv) can be copied next to a private image dataset.

Run every profile over the exact same images:

```sh
./logistics_vision_benchmark \
  --dataset /data/vision-benchmark/images \
  --manifest /data/vision-benchmark/manifest.csv \
  --iterations 5 \
  --output /tmp/vision-benchmark.csv
```

To include learned SR:

```sh
./logistics_vision_benchmark \
  --dataset /data/vision-benchmark/images \
  --manifest /data/vision-benchmark/manifest.csv \
  --iterations 5 \
  --fsrcnn-model /opt/logistics/models/FSRCNN_x2.pb
```

The benchmark performs an ablation study with these profiles:

- `baseline`: original pixels only;
- `rectification`: adds perspective correction;
- `rectification_contrast`: adds CLAHE and sharpening;
- `barcode_detection_bicubic_sr_x2`: isolates box-ROI SR for barcode-region detection;
- `barcode_decode_bicubic_sr_x2`: isolates rectified barcode-ROI SR for decoding;
- `full_bicubic_sr_x2`: enables both ROI-only bicubic fallbacks;
- `full_fsrcnn_sr_x2`: enables both learned SR fallbacks when a model path is supplied.

CSV output reports box, barcode-region, decode, and exact EAN-13 accuracy rates. It also reports average total latency
and time spent in box detection, barcode detection, perspective correction, contrast enhancement, SR, and decoding.
Use real production captures with clear, blurred, angled, dark, partially occluded, and motion-blurred groups; synthetic
images alone do not represent camera and compression artifacts.
