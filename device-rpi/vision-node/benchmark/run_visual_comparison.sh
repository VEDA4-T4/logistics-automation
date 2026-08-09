#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../../.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build-vision-benchmark"
OUTPUT_DIR="/tmp/logistics-vision-sr-comparison"
ITERATIONS=1
WARMUP=1
DURATION_SECONDS=0
VISUAL_LIMIT=10
FSRCNN_MODEL=""
PROFILE=""
LABEL=""
LOAD_PID=""
LOAD_LOG=""
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
  --warmup N             Untimed warm-up iterations (default: 1)
  --duration-seconds N   Continue each profile for at least N seconds
  --profile NAME         Run only one benchmark profile
  --label NAME           Suffix CSV/report names, for example operational
  --load-pid PID         Require a concurrent workload process to stay alive
  --load-log FILE        Verify MQTT and HTTP success logs during operational run
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

require_vision_load_process() {
    if ! kill -0 "$1" 2>/dev/null; then
        echo "[sr-visual-test][ERROR] Load process is not running: $1" >&2
        exit 2
    fi
    local executable
    executable="$(readlink -f "/proc/$1/exe" 2>/dev/null || true)"
    if [[ "${executable##*/}" != "logistics_vision_node" ]]; then
        echo "[sr-visual-test][ERROR] Load PID is not logistics_vision_node: $1" >&2
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
        --warmup)
            require_value "$@"
            if [[ ! "$2" =~ ^[0-9]+$ ]]; then
                echo "[sr-visual-test][ERROR] --warmup must be a non-negative integer" >&2
                exit 2
            fi
            WARMUP="$2"
            shift 2
            ;;
        --duration-seconds)
            require_value "$@"
            if [[ ! "$2" =~ ^[0-9]+$ ]]; then
                echo "[sr-visual-test][ERROR] --duration-seconds must be a non-negative integer" >&2
                exit 2
            fi
            DURATION_SECONDS="$2"
            shift 2
            ;;
        --profile)
            require_value "$@"
            PROFILE="$2"
            shift 2
            ;;
        --label)
            require_value "$@"
            if [[ ! "$2" =~ ^[A-Za-z0-9._-]+$ ]]; then
                echo "[sr-visual-test][ERROR] --label contains unsupported characters" >&2
                exit 2
            fi
            LABEL="$2"
            shift 2
            ;;
        --load-pid)
            require_value "$@"
            require_positive_integer "$1" "$2"
            LOAD_PID="$2"
            shift 2
            ;;
        --load-log)
            require_value "$@"
            LOAD_LOG="$2"
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
if [[ "${LABEL}" == "operational" && (-z "${LOAD_PID}" || -z "${LOAD_LOG}") ]]; then
    echo "[sr-visual-test][ERROR] --label operational requires --load-pid and --load-log" >&2
    exit 2
fi
if [[ "${LABEL}" == "operational" && -z "${PROFILE}" ]]; then
    echo "[sr-visual-test][ERROR] --label operational requires --profile" >&2
    exit 2
fi
if [[ -n "${LOAD_PID}" ]]; then
    require_vision_load_process "${LOAD_PID}"
fi
if [[ -n "${LOAD_LOG}" && ! -f "${LOAD_LOG}" ]]; then
    echo "[sr-visual-test][ERROR] Load log not found: ${LOAD_LOG}" >&2
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

suffix="${LABEL:+-${LABEL}}"
VISUAL_DIR="${OUTPUT_DIR}/visuals${suffix}"
CSV_PATH="${OUTPUT_DIR}/benchmark${suffix}.csv"
SUMMARY_PATH="${OUTPUT_DIR}/summary${suffix}.md"
rm -rf -- "${VISUAL_DIR}"
mkdir -p -- "${VISUAL_DIR}"

model_args=()
if [[ -n "${FSRCNN_MODEL}" ]]; then
    model_args=(--fsrcnn-model "${FSRCNN_MODEL}")
fi
profile_args=()
if [[ -n "${PROFILE}" ]]; then
    profile_args=(--profile "${PROFILE}")
fi
load_log_start_lines=0
if [[ -n "${LOAD_LOG}" ]]; then
    load_log_start_lines="$(wc -l < "${LOAD_LOG}")"
fi

"${BENCHMARK}" \
    --dataset "${DATASET_DIR}" \
    --manifest "${MANIFEST_PATH}" \
    --iterations "${ITERATIONS}" \
    --warmup "${WARMUP}" \
    --duration-seconds "${DURATION_SECONDS}" \
    --output "${CSV_PATH}" \
    --visual-output "${VISUAL_DIR}" \
    --visual-limit "${VISUAL_LIMIT}" \
    "${profile_args[@]}" \
    "${model_args[@]}"

if [[ -n "${LOAD_PID}" ]]; then
    require_vision_load_process "${LOAD_PID}"
fi
if [[ -n "${LOAD_LOG}" ]]; then
    for marker in "MQTT result publication completed" "HTTP image upload confirmed"; do
        if ! awk -v start="${load_log_start_lines}" -v marker="${marker}" \
            'NR > start && index($0, marker) { found = 1 } END { exit !found }' "${LOAD_LOG}"; then
            echo "[sr-visual-test][ERROR] Operational load log is missing: ${marker}" >&2
            exit 1
        fi
    done
fi

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

{
    echo "# Vision Benchmark Summary"
    echo
    echo "- Label: ${LABEL:-baseline}"
    echo "- Warm-up iterations: ${WARMUP}"
    echo "- Minimum duration per profile: ${DURATION_SECONDS} seconds"
    echo "- Concurrent load PID: ${LOAD_PID:-none}"
    echo "- Concurrent load log: ${LOAD_LOG:-none}"
    echo
    awk -F, '
        NR == 1 {
            for (field_index = 1; field_index <= NF; ++field_index) column[$field_index] = field_index
            print "| Profile | Accuracy % | p95 ms | p99 ms | FPS | CPU % | Avg RSS KB | Peak RSS KB | FPS change % | RSS growth KB |"
            print "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
            next
        }
        {
            profile = $column["profile"]
            accuracy = $column["accuracy_percent"] + 0
            p95 = $column["p95_total_ms"] + 0
            printf "| %s | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f |\n", profile,
                accuracy, p95, $column["p99_total_ms"], $column["throughput_fps"], $column["cpu_percent"],
                $column["average_rss_kb"], $column["peak_rss_kb"], $column["throughput_change_percent"],
                $column["rss_growth_kb"]
            if (best_profile == "" || accuracy > best_accuracy || (accuracy == best_accuracy && p95 < best_p95)) {
                best_profile = profile
                best_accuracy = accuracy
                best_p95 = p95
            }
        }
        END {
            printf "\nRecommended profile: `%s` (highest accuracy, then lowest p95 latency).\n", best_profile
        }
    ' "${CSV_PATH}"
} > "${SUMMARY_PATH}"

echo "[sr-visual-test] Completed"
echo "[sr-visual-test] CSV: ${CSV_PATH}"
echo "[sr-visual-test] Summary: ${SUMMARY_PATH}"
echo "[sr-visual-test] Comparison images: ${VISUAL_DIR} (${#visual_outputs[@]} file(s))"
echo "[sr-visual-test] Open the PNG files at 100% zoom."
