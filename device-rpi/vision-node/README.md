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
failure_frame_capture_enabled=true
failure_frame_directory=/tmp/logistics-vision-failures
maximum_failure_frames=200
failure_frame_jpeg_quality=90
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
6. Camera, encoding, and upload failures are reported with `ERROR_OCCURRED`. MQTT publication failures keep the
   assigned result in `RESULT_PENDING` and resume from the first unsent message after reconnecting.

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

When final barcode recognition fails for an assigned work, the unannotated camera frame is saved separately under
`failure_frame_directory`. Only `maximum_failure_frames` JPEG files are retained, so this temporary benchmark input
cannot grow without bound. A storage failure is logged as a warning and does not replace the MQTT recognition result.

Failure results include enough metadata to identify the failed stage:

| Failure | `errorCode` | `failureStage` |
| --- | --- | --- |
| Barcode region missing | `ERR-VISION-BARCODE-REGION-NOT-DETECTED` | `BARCODE_DETECTION` |
| Region found but EAN-13 decode failed | `ERR-VISION-BARCODE-DECODE-FAILED` | `BARCODE_DECODE` |
| Camera cannot open | `ERR-VISION-CAMERA-OPEN-FAILED` | `ERROR_OCCURRED` event |
| Camera frame stream lost | `ERR-VISION-CAMERA-FRAME-UNAVAILABLE` | `ERROR_OCCURRED` event |
| JPEG encode, capture, or HTTP upload failure | `ERR-VISION-IMAGE-*` | `ERROR_OCCURRED` event |

For an MQTT recovery check, start a work, stop the broker before the vision result is published, and restart it. The
node must remain in `RESULT_PENDING` while disconnected, publish only the unsent result messages after reconnect, and
then return to `WAITING_FOR_PRODUCT`. Replaying the same `WORK_CREATED` after completion must not produce a second
result for that `workId`.

No-box frames before `BOX_DETECTED` are not reported as a work failure because the server has not assigned a `workId`
yet. The node keeps scanning without raising a system error. A box-arrival timeout requires an upstream sensor event
that creates an expected work before vision processing; it must not be inferred from ordinary empty camera frames.

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
  --output /tmp/vision-benchmark.csv \
  --visual-output /tmp/vision-sr-comparisons \
  --visual-limit 10
```

Each visual comparison PNG shows the same detected barcode ROI (or box ROI when a barcode ROI is unavailable) at the
same 2x dimensions:

- `ORIGINAL x2 (NEAREST)` preserves the source pixels so blur and compression damage remain visible;
- `BICUBIC SR x2` is the exact dependency-free SR path used by the node;
- `FSRCNN SR x2` is added when `--fsrcnn-model` is supplied.

Open the PNGs in `--visual-output` at 100% zoom. Nearest-neighbor output is intentionally used for the original panel;
using a smooth viewer resize there would hide the difference that the comparison is intended to expose.

The helper script builds the benchmark, runs the automated SR preview tests, generates the CSV, and verifies that the
comparison PNGs were created:

```sh
./device-rpi/vision-node/benchmark/run_visual_comparison.sh \
  --dataset /data/vision-benchmark/images \
  --manifest /data/vision-benchmark/manifest.csv \
  --output-dir /tmp/vision-sr-test \
  --iterations 5 \
  --visual-limit 10
```

Pass `--fsrcnn-model /opt/logistics/models/FSRCNN_x2.pb` to include the learned SR panel. Use `--skip-build` when the
benchmark has already been built in `build-vision-benchmark`.

For a Pi 4 reproduction, this single command builds, runs the preview checks, records the warm-up-excluded metrics, and
writes the SR comparison images, CSV, and summary under `/tmp/vision-sr-pi4`:

```sh
./device-rpi/vision-node/benchmark/run_visual_comparison.sh \
  --dataset /data/vision-benchmark/images \
  --manifest /data/vision-benchmark/manifest.csv \
  --output-dir /tmp/vision-sr-pi4 \
  --iterations 5 --warmup 5 --label baseline
```

The CSV reports accuracy, p50/p95/p99 total latency, FPS, CPU, average/peak RSS, and per-stage timings. The summary keeps
the comparison metrics and selects the highest-accuracy profile, breaking ties by p95 latency.
For a long-run FPS/RSS check, use a production-sized capture set and keep each profile active for at least 30 minutes:

```sh
./device-rpi/vision-node/benchmark/run_visual_comparison.sh \
  --dataset /data/vision-benchmark/images \
  --manifest /data/vision-benchmark/manifest.csv \
  --output-dir /tmp/vision-sr-soak \
  --profile full_bicubic_sr_x2 --iterations 5 --warmup 20 \
  --duration-seconds 1800 --label soak
```

Compare `first_half_fps`, `last_half_fps`, `throughput_change_percent`, and `rss_growth_kb` in the soak CSV before
accepting the profile.

For the transport-load comparison, first run the recommended profile without the live node:

```sh
./device-rpi/vision-node/benchmark/run_visual_comparison.sh \
  --dataset /data/vision-benchmark/images --manifest /data/vision-benchmark/manifest.csv \
  --output-dir /tmp/vision-sr-operational --profile full_bicubic_sr_x2 --label baseline
```

Then start the configured node in another terminal, keep the central server, broker, and upload endpoint running, and
feed products through the camera so MQTT results and HTTP images are actually sent:

```sh
./logistics_vision_node --headless --config runtime/vision-node/vision-node.ini \
  2>&1 | tee /tmp/vision-operational.log
```

```sh
vision_pid="$(pgrep -n logistics_vision_node)"
./device-rpi/vision-node/benchmark/run_visual_comparison.sh \
  --dataset /data/vision-benchmark/images --manifest /data/vision-benchmark/manifest.csv \
  --output-dir /tmp/vision-sr-operational --profile full_bicubic_sr_x2 \
  --label operational --load-pid "${vision_pid}" --load-log /tmp/vision-operational.log
```

The operational run succeeds only when the real node stays alive and the measurement window adds both an MQTT result
publication and a confirmed HTTP image upload to the log. Compare `benchmark-baseline.csv` with
`benchmark-operational.csv` and retain the separate `visuals-baseline/` and `visuals-operational/` directories.

To include learned SR:

```sh
./logistics_vision_benchmark \
  --dataset /data/vision-benchmark/images \
  --manifest /data/vision-benchmark/manifest.csv \
  --iterations 5 \
  --fsrcnn-model /opt/logistics/models/FSRCNN_x2.pb \
  --visual-output /tmp/vision-sr-comparisons
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
