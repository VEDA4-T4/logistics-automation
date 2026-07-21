#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
build_dir="${LOGISTICS_BUILD_DIR:-${repo_root}/build-vision}"
runtime_dir="${LOGISTICS_RUNTIME_DIR:-${repo_root}/runtime/vision-node}"
config_path="${LOGISTICS_CONFIG_PATH:-${runtime_dir}/vision-node.ini}"
central_host="${LOGISTICS_CENTRAL_HOST:-}"
mqtt_host="${LOGISTICS_MQTT_HOST:-${central_host}}"
upload_token="${LOGISTICS_UPLOAD_TOKEN:-}"
device_id="${LOGISTICS_DEVICE_ID:-PI-VISION-01}"
node_name="${LOGISTICS_NODE_NAME:-vision-node-01}"
device_ip="${LOGISTICS_DEVICE_IP:-}"
force_config="${LOGISTICS_FORCE_CONFIG:-0}"
install_opencv="${LOGISTICS_INSTALL_OPENCV:-0}"
install_dependencies="${LOGISTICS_INSTALL_DEPENDENCIES:-0}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must run on the vision Linux/Raspberry Pi host." >&2
    exit 2
fi
if [[ (! -e "${config_path}" || "${force_config}" == "1") &&
      (-z "${central_host}" || -z "${upload_token}") ]]; then
    echo "LOGISTICS_CENTRAL_HOST and LOGISTICS_UPLOAD_TOKEN must be set." >&2
    exit 2
fi
if [[ (! -e "${config_path}" || "${force_config}" == "1") && -z "${device_ip}" ]]; then
    device_ip="$(hostname -I | awk '{print $1}')"
fi
if [[ (! -e "${config_path}" || "${force_config}" == "1") && -z "${device_ip}" ]]; then
    echo "Could not detect the vision node IP; set LOGISTICS_DEVICE_IP." >&2
    exit 2
fi

sudo_command=()
if [[ "${EUID}" -ne 0 ]]; then
    sudo_command=(sudo)
fi

if [[ "${install_dependencies}" == "1" ]]; then
    "${sudo_command[@]}" apt-get update
    "${sudo_command[@]}" apt-get install -y \
        build-essential cmake ninja-build pkg-config curl \
        libmosquitto-dev libcurl4-openssl-dev libssl-dev nlohmann-json3-dev
fi

opencv_version="$(pkg-config --modversion opencv4 2>/dev/null || true)"
if [[ "${opencv_version}" != "4.10.0" ]]; then
    if [[ "${install_opencv}" == "1" ]]; then
        "${sudo_command[@]}" bash "${repo_root}/.github/scripts/install-opencv.sh" 4.10.0 /usr/local
    else
        echo "OpenCV 4.10.0 is required; found '${opencv_version:-none}'." >&2
        echo "Re-run with LOGISTICS_INSTALL_OPENCV=1 to build and install it." >&2
        exit 3
    fi
fi

install -d -m 0750 "${runtime_dir}" "$(dirname -- "${config_path}")"
if [[ -e "${config_path}" && "${force_config}" != "1" ]]; then
    echo "Keeping existing config: ${config_path}"
else
    temporary_config="$(mktemp)"
    trap 'rm -f -- "${temporary_config:-}"' EXIT
    cat >"${temporary_config}" <<EOF
[device]
device_id=${device_id}
node_name=${node_name}
ip_address=${device_ip}

[mqtt]
host=${mqtt_host}
port=1883
client_id=${device_id}
username=
password=
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=true

[log_upload]
enabled=false

[image_upload]
enabled=true
endpoint_url=http://${central_host}:8080/api/v1/uploads/images
bearer_token=${upload_token}
ca_certificate=
request_timeout_seconds=30
maximum_attempts=5
initial_backoff_seconds=1
maximum_backoff_seconds=60
allow_insecure_http=true
EOF
    install -m 0600 "${temporary_config}" "${config_path}"
    echo "Created config: ${config_path}"
fi

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
    -DLOGISTICS_BUILD_DEVICE_NODES=ON \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON
cmake --build "${build_dir}" --target logistics_vision_node

echo
echo "Vision node setup complete."
echo "Run: ${build_dir}/device-rpi/logistics_vision_node --config ${config_path} --camera 0"
