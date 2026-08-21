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
mqtt_port="${LOGISTICS_MQTT_PORT:-8883}"
mqtt_username="${LOGISTICS_MQTT_USERNAME:-${device_id}}"
mqtt_password="${LOGISTICS_MQTT_PASSWORD:-}"
mqtt_tls_enabled="${LOGISTICS_MQTT_TLS_ENABLED:-true}"
mqtt_ca_certificate="${LOGISTICS_MQTT_CA_CERTIFICATE:-/etc/logistics/tls/ca.crt}"
node_name="${LOGISTICS_NODE_NAME:-input-node-01}"
device_ip="${LOGISTICS_DEVICE_IP:-}"
uart_device="${LOGISTICS_UART_DEVICE:-/dev/vedauart}"
force_config="${LOGISTICS_FORCE_CONFIG:-0}"
install_dependencies="${LOGISTICS_INSTALL_DEPENDENCIES:-0}"

reject_line_breaks() {
    [[ "$1" != *$'\n'* && "$1" != *$'\r'* ]]
}

validate_mqtt_settings() {
    local host=$1 port=$2 username=$3 password=$4 tls_enabled=$5 ca_certificate=$6

    if [[ -z "${host}" || -z "${username}" || -z "${password}" ]]; then
        echo "MQTT host, username, and password must be set." >&2
        return 1
    fi
    if ! reject_line_breaks "${host}" || ! reject_line_breaks "${username}" ||
        ! reject_line_breaks "${password}" || ! reject_line_breaks "${ca_certificate}"; then
        echo "MQTT settings must not contain line breaks." >&2
        return 1
    fi
    if [[ ! "${port}" =~ ^[1-9][0-9]{0,4}$ ]] || ((port > 65535)); then
        echo "LOGISTICS_MQTT_PORT must be an integer from 1 to 65535." >&2
        return 1
    fi
    if [[ "${tls_enabled}" != true && "${tls_enabled}" != false ]]; then
        echo "LOGISTICS_MQTT_TLS_ENABLED must be true or false." >&2
        return 1
    fi
    if [[ "${tls_enabled}" == true && -z "${ca_certificate}" ]]; then
        echo "LOGISTICS_MQTT_CA_CERTIFICATE is required when MQTT TLS is enabled." >&2
        return 1
    fi
}

run_self_check() {
    validate_mqtt_settings mqtt.example 8883 PI-INPUT-01 secret true /etc/logistics/tls/ca.crt
    validate_mqtt_settings mqtt.example 1883 PI-INPUT-01 secret false ""
    ! validate_mqtt_settings mqtt.example 65536 PI-INPUT-01 secret true /etc/logistics/tls/ca.crt 2>/dev/null
    ! validate_mqtt_settings mqtt.example 8883 PI-INPUT-01 "" true /etc/logistics/tls/ca.crt 2>/dev/null
    ! validate_mqtt_settings mqtt.example 8883 PI-INPUT-01 secret 1 /etc/logistics/tls/ca.crt 2>/dev/null
    ! validate_mqtt_settings mqtt.example 8883 PI-INPUT-01 secret true "" 2>/dev/null
    ! validate_mqtt_settings mqtt.example 8883 $'PI-INPUT-01\ninvalid' secret true /etc/logistics/tls/ca.crt \
        2>/dev/null
    echo "$(basename -- "${BASH_SOURCE[0]}") MQTT security self-check passed."
}

if [[ "${1:-}" == "--self-check" ]]; then
    run_self_check
    exit 0
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must run on the input Linux/Raspberry Pi host." >&2
    exit 2
fi
if [[ (! -e "${config_path}" || "${force_config}" == "1") && -z "${mqtt_host}" ]]; then
    echo "LOGISTICS_CENTRAL_HOST (or LOGISTICS_MQTT_HOST) must be set." >&2
    exit 2
fi
if [[ ! -e "${config_path}" || "${force_config}" == "1" ]]; then
    validate_mqtt_settings "${mqtt_host}" "${mqtt_port}" "${mqtt_username}" "${mqtt_password}" \
        "${mqtt_tls_enabled}" "${mqtt_ca_certificate}" || exit 2
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
port=${mqtt_port}
client_id=${device_id}
username=${mqtt_username}
password=${mqtt_password}
tls_enabled=${mqtt_tls_enabled}
ca_certificate=${mqtt_ca_certificate}
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
