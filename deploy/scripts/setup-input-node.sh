#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
build_dir="${LOGISTICS_BUILD_DIR:-${repo_root}/build-input}"
runtime_dir="${LOGISTICS_RUNTIME_DIR:-${repo_root}/runtime/input-node}"
config_path="${LOGISTICS_CONFIG_PATH:-${runtime_dir}/input-node.ini}"
central_host="${LOGISTICS_CENTRAL_HOST:-}"
mqtt_host="${LOGISTICS_MQTT_HOST:-${central_host}}"
device_id="${LOGISTICS_DEVICE_ID:-PI-INPUT-01}"
node_name="${LOGISTICS_NODE_NAME:-input-node-01}"
device_ip="${LOGISTICS_DEVICE_IP:-}"
uart_device="${LOGISTICS_UART_DEVICE:-/dev/vedauart}"
force_config="${LOGISTICS_FORCE_CONFIG:-0}"
install_dependencies="${LOGISTICS_INSTALL_DEPENDENCIES:-0}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must run on the input Linux/Raspberry Pi host." >&2
    exit 2
fi
if [[ (! -e "${config_path}" || "${force_config}" == "1") && -z "${mqtt_host}" ]]; then
    echo "LOGISTICS_CENTRAL_HOST (or LOGISTICS_MQTT_HOST) must be set." >&2
    exit 2
fi
if [[ (! -e "${config_path}" || "${force_config}" == "1") && -z "${device_ip}" ]]; then
    device_ip="$(hostname -I | awk '{print $1}')"
fi
if [[ (! -e "${config_path}" || "${force_config}" == "1") && -z "${device_ip}" ]]; then
    echo "Could not detect the input node IP; set LOGISTICS_DEVICE_IP." >&2
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
EOF
    install -m 0600 "${temporary_config}" "${config_path}"
    echo "Created config: ${config_path}"
fi

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
    -DLOGISTICS_BUILD_DEVICE_NODES=ON \
    -DLOGISTICS_BUILD_INPUT_NODE=ON \
    -DLOGISTICS_BUILD_VISION_NODE=OFF \
    -DLOGISTICS_BUILD_SORTING_NODE=OFF \
    -DLOGISTICS_BUILD_LINETRACER_NODE=OFF \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON
cmake --build "${build_dir}" --target logistics_input_node

echo
echo "Input node setup complete."
echo "The STM32 UART character device defaults to ${uart_device}."
echo "Run: LOGISTICS_UART_DEVICE=${uart_device} \\"
echo "     ${build_dir}/device-rpi/logistics_input_node ${config_path}"
