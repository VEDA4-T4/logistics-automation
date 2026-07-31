#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../../.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build-vision-benchmark"
OUTPUT_DIR="/tmp/logistics-vision-sr-comparison"
ITERATIONS=1
VISUAL_LIMIT=10
FSRCNN_MODEL=""
SKIP_BUILD=false
DATASET_DIR=""
MANIFEST_PATH=""

usage() {
    cat <<'EOF'
Usage:
  run_visual_comparison.sh --dataset DIR --manifest FILE [options]

Required:
  --dataset DIR          Directory containing benchmark images
  --manifest FILE        CSV containing filename,ean13 rows

Options:
  --output-dir DIR       CSV and comparison PNG directory
                         (default: /tmp/logistics-vision-sr-comparison)
  --iterations N         Benchmark iterations (default: 1)
  --visual-limit N       Maximum comparison PNG count (default: 10)
  --fsrcnn-model FILE    Add an FSRCNN comparison panel
  --build-dir DIR        CMake build directory
  --skip-build           Use an already-built benchmark executable
  -h, --help             Show this help
EOF
}

require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        echo "[sr-visual-test][ERROR] Missing value for $1" >&2
        usage >&2
        exit 2
    fi
}

require_positive_integer() {
    if [[ ! "$2" =~ ^[1-9][0-9]*$ ]]; then
        echo "[sr-visual-test][ERROR] $1 must be a positive integer" >&2
        exit 2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dataset)
            require_value "$@"
            DATASET_DIR="$2"
            shift 2
            ;;
        --manifest)
            require_value "$@"
            MANIFEST_PATH="$2"
            shift 2
            ;;
        --output-dir)
            require_value "$@"
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --iterations)
            require_value "$@"
            require_positive_integer "$1" "$2"
            ITERATIONS="$2"
            shift 2
            ;;
        --visual-limit)
            require_value "$@"
            require_positive_integer "$1" "$2"
            VISUAL_LIMIT="$2"
            shift 2
            ;;
        --fsrcnn-model)
            require_value "$@"
            FSRCNN_MODEL="$2"
            shift 2
            ;;
        --build-dir)
            require_value "$@"
            BUILD_DIR="$2"
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[sr-visual-test][ERROR] Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "${DATASET_DIR}" || -z "${MANIFEST_PATH}" ]]; then
    echo "[sr-visual-test][ERROR] --dataset and --manifest are required" >&2
    usage >&2
    exit 2
fi
if [[ ! -d "${DATASET_DIR}" ]]; then
    echo "[sr-visual-test][ERROR] Dataset directory not found: ${DATASET_DIR}" >&2
    exit 2
fi
if [[ ! -f "${MANIFEST_PATH}" ]]; then
    echo "[sr-visual-test][ERROR] Manifest not found: ${MANIFEST_PATH}" >&2
    exit 2
fi
if [[ -n "${FSRCNN_MODEL}" && ! -f "${FSRCNN_MODEL}" ]]; then
    echo "[sr-visual-test][ERROR] FSRCNN model not found: ${FSRCNN_MODEL}" >&2
    exit 2
fi

if [[ "${SKIP_BUILD}" == false ]]; then
    if ! command -v cmake >/dev/null 2>&1; then
        echo "[sr-visual-test][ERROR] cmake is required" >&2
        exit 1
    fi

    generator_args=()
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
        generator_args=(-G Ninja)
    fi
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${generator_args[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=ON \
        -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
        -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
        -DLOGISTICS_BUILD_DEVICE_NODES=ON \
        -DLOGISTICS_BUILD_INPUT_NODE=OFF \
        -DLOGISTICS_BUILD_VISION_NODE=ON \
        -DLOGISTICS_BUILD_SORTING_NODE=OFF \
        -DLOGISTICS_BUILD_LINETRACER_NODE=OFF \
        -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=OFF
    cmake --build "${BUILD_DIR}" \
        --target logistics_vision_benchmark vision_super_resolution_preview_test \
        --parallel
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -R vision_super_resolution_preview_test
    ctest --test-dir "${BUILD_DIR}" --output-on-failure -R vision_benchmark_visual_output
fi

BENCHMARK="${BUILD_DIR}/device-rpi/logistics_vision_benchmark"
if [[ ! -x "${BENCHMARK}" ]]; then
    echo "[sr-visual-test][ERROR] Benchmark executable not found: ${BENCHMARK}" >&2
    exit 1
fi

VISUAL_DIR="${OUTPUT_DIR}/visuals"
CSV_PATH="${OUTPUT_DIR}/benchmark.csv"
rm -rf -- "${VISUAL_DIR}"
mkdir -p -- "${VISUAL_DIR}"

model_args=()
if [[ -n "${FSRCNN_MODEL}" ]]; then
    model_args=(--fsrcnn-model "${FSRCNN_MODEL}")
fi

"${BENCHMARK}" \
    --dataset "${DATASET_DIR}" \
    --manifest "${MANIFEST_PATH}" \
    --iterations "${ITERATIONS}" \
    --output "${CSV_PATH}" \
    --visual-output "${VISUAL_DIR}" \
    --visual-limit "${VISUAL_LIMIT}" \
    "${model_args[@]}"

shopt -s nullglob
visual_outputs=("${VISUAL_DIR}"/*-sr-comparison.png)
if [[ ${#visual_outputs[@]} -eq 0 ]]; then
    echo "[sr-visual-test][ERROR] No comparison images were generated" >&2
    exit 1
fi
if [[ ! -s "${CSV_PATH}" ]]; then
    echo "[sr-visual-test][ERROR] Benchmark CSV was not generated" >&2
    exit 1
fi

echo "[sr-visual-test] Completed"
echo "[sr-visual-test] CSV: ${CSV_PATH}"
echo "[sr-visual-test] Comparison images: ${VISUAL_DIR} (${#visual_outputs[@]} file(s))"
echo "[sr-visual-test] Open the PNG files at 100% zoom."
