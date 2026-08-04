#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"

build_dir="${LOGISTICS_BUILD_DIR:-${repo_root}/build-sorting}"
install_prefix="${LOGISTICS_INSTALL_PREFIX:-/opt/logistics-automation}"
runtime_dir="${LOGISTICS_RUNTIME_DIR:-/var/lib/logistics}"
config_path="${LOGISTICS_CONFIG_PATH:-/etc/logistics/sorting-node.ini}"
service_name="logistics-sorting-node.service"

central_host="${LOGISTICS_CENTRAL_HOST:-}"
mqtt_host="${LOGISTICS_MQTT_HOST:-${central_host}}"
device_id="${LOGISTICS_DEVICE_ID:-PI-SORTING-01}"
node_name="${LOGISTICS_NODE_NAME:-sorting-node-01}"
device_ip="${LOGISTICS_DEVICE_IP:-}"

mqtt_port="${LOGISTICS_MQTT_PORT:-8883}"
mqtt_username="${LOGISTICS_MQTT_USERNAME:-${device_id}}"
mqtt_password="${LOGISTICS_MQTT_PASSWORD:-}"
mqtt_tls_enabled="${LOGISTICS_MQTT_TLS_ENABLED:-true}"
mqtt_ca_certificate="${LOGISTICS_MQTT_CA_CERTIFICATE:-/etc/logistics/tls/ca.crt}"

uart_device="${LOGISTICS_UART_DEVICE:-/dev/vedauart}"
sorting_default_speed="${LOGISTICS_SORTING_DEFAULT_SPEED:-50}"

force_config="${LOGISTICS_FORCE_CONFIG:-0}"
install_dependencies="${LOGISTICS_INSTALL_DEPENDENCIES:-0}"

reject_line_breaks() {
    [[ "$1" != *$'\n'* && "$1" != *$'\r'* ]]
}

validate_mqtt_settings() {
    local host=$1
    local port=$2
    local username=$3
    local password=$4
    local tls_enabled=$5
    local ca_certificate=$6

    if [[ -z "${host}" || -z "${username}" || -z "${password}" ]]; then
        echo "MQTT host, username, and password must be set." >&2
        return 1
    fi

    if ! reject_line_breaks "${host}" ||
        ! reject_line_breaks "${username}" ||
        ! reject_line_breaks "${password}" ||
        ! reject_line_breaks "${ca_certificate}"; then
        echo "MQTT settings must not contain line breaks." >&2
        return 1
    fi

    if [[ ! "${port}" =~ ^[1-9][0-9]{0,4}$ ]] ||
        ((port > 65535)); then
        echo "LOGISTICS_MQTT_PORT must be an integer from 1 to 65535." >&2
        return 1
    fi

    if [[ "${tls_enabled}" != "true" &&
          "${tls_enabled}" != "false" ]]; then
        echo "LOGISTICS_MQTT_TLS_ENABLED must be true or false." >&2
        return 1
    fi

    if [[ "${tls_enabled}" == "true" &&
          -z "${ca_certificate}" ]]; then
        echo "LOGISTICS_MQTT_CA_CERTIFICATE is required when MQTT TLS is enabled." >&2
        return 1
    fi
}

validate_sorting_speed() {
    local speed=$1

    if [[ ! "${speed}" =~ ^[0-9]+$ ]] ||
        ((speed < 1 || speed > 100)); then
        echo "LOGISTICS_SORTING_DEFAULT_SPEED must be an integer from 1 to 100." >&2
        return 1
    fi
}

run_self_check() {
    validate_mqtt_settings \
        mqtt.example \
        8883 \
        PI-SORTING-01 \
        secret \
        true \
        /etc/logistics/tls/ca.crt

    validate_mqtt_settings \
        mqtt.example \
        1883 \
        PI-SORTING-01 \
        secret \
        false \
        ""

    validate_sorting_speed 50

    ! validate_mqtt_settings \
        mqtt.example \
        65536 \
        PI-SORTING-01 \
        secret \
        true \
        /etc/logistics/tls/ca.crt \
        2>/dev/null

    ! validate_mqtt_settings \
        mqtt.example \
        8883 \
        PI-SORTING-01 \
        "" \
        true \
        /etc/logistics/tls/ca.crt \
        2>/dev/null

    ! validate_sorting_speed 0 2>/dev/null
    ! validate_sorting_speed 101 2>/dev/null

    echo "$(basename -- "${BASH_SOURCE[0]}") self-check passed."
}

if [[ "${1:-}" == "--self-check" ]]; then
    run_self_check
    exit 0
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must run on the sorting Linux/Raspberry Pi host." >&2
    exit 2
fi

sudo_command=()
if [[ "${EUID}" -ne 0 ]]; then
    sudo_command=(sudo)
fi

config_exists=0
if "${sudo_command[@]}" test -e "${config_path}"; then
    config_exists=1
fi

write_config=0
if [[ "${config_exists}" == "0" ||
      "${force_config}" == "1" ]]; then
    write_config=1
fi

if [[ "${write_config}" == "1" &&
      -z "${mqtt_host}" ]]; then
    echo "LOGISTICS_CENTRAL_HOST (or LOGISTICS_MQTT_HOST) must be set." >&2
    exit 2
fi

if [[ "${write_config}" == "1" ]]; then
    validate_mqtt_settings \
        "${mqtt_host}" \
        "${mqtt_port}" \
        "${mqtt_username}" \
        "${mqtt_password}" \
        "${mqtt_tls_enabled}" \
        "${mqtt_ca_certificate}" ||
        exit 2

    validate_sorting_speed "${sorting_default_speed}" ||
        exit 2
fi

if [[ "${write_config}" == "1" &&
      -z "${device_ip}" ]]; then
    device_ip="$(hostname -I | awk '{print $1}')"
fi

if [[ "${write_config}" == "1" &&
      -z "${device_ip}" ]]; then
    echo "Could not detect the sorting node IP; set LOGISTICS_DEVICE_IP." >&2
    exit 2
fi

if [[ "${uart_device}" != "/dev/vedauart" ]]; then
    echo "The systemd unit currently requires /dev/vedauart." >&2
    exit 2
fi

if [[ "${install_dependencies}" == "1" ]]; then
    "${sudo_command[@]}" apt-get update

    "${sudo_command[@]}" apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        curl \
        libmosquitto-dev \
        libcurl4-openssl-dev \
        libssl-dev \
        nlohmann-json3-dev
fi

if ! getent group logistics >/dev/null; then
    "${sudo_command[@]}" groupadd \
        --system \
        logistics
fi

if ! getent passwd logistics >/dev/null; then
    "${sudo_command[@]}" useradd \
        --system \
        --gid logistics \
        --home-dir /var/lib/logistics \
        --shell /usr/sbin/nologin \
        logistics
fi

"${sudo_command[@]}" install \
    -d \
    -o root \
    -g logistics \
    -m 0750 \
    "$(dirname -- "${config_path}")"

if [[ "${write_config}" != "1" ]]; then
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

[sorting]
default_speed=${sorting_default_speed}
EOF

    "${sudo_command[@]}" install \
        -o root \
        -g logistics \
        -m 0640 \
        "${temporary_config}" \
        "${config_path}"

    echo "Created config: ${config_path}"
fi

"${sudo_command[@]}" chown \
    root:logistics \
    "${config_path}"

"${sudo_command[@]}" chmod \
    0640 \
    "${config_path}"

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
    -DLOGISTICS_BUILD_DEVICE_NODES=ON \
    -DLOGISTICS_BUILD_INPUT_NODE=OFF \
    -DLOGISTICS_BUILD_VISION_NODE=OFF \
    -DLOGISTICS_BUILD_SORTING_NODE=ON \
    -DLOGISTICS_BUILD_LINETRACER_NODE=OFF \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON

cmake --build "${build_dir}" \
    --target logistics_sorting_node

"${sudo_command[@]}" cmake --install "${build_dir}" \
    --prefix "${install_prefix}"

installed_unit="${install_prefix}/lib/systemd/system/${service_name}"

if ! "${sudo_command[@]}" test -f "${installed_unit}"; then
    echo "Installed systemd unit was not found: ${installed_unit}" >&2
    exit 4
fi

"${sudo_command[@]}" install \
    -o root \
    -g root \
    -m 0644 \
    "${installed_unit}" \
    "/etc/systemd/system/${service_name}"

"${sudo_command[@]}" systemctl daemon-reload
"${sudo_command[@]}" systemctl enable "${service_name}"
"${sudo_command[@]}" systemctl restart "${service_name}"

echo
echo "Sorting node setup complete."
echo "Binary: ${install_prefix}/bin/logistics_sorting_node"
echo "Config: ${config_path}"
echo "UART: ${uart_device}"
echo "Service: ${service_name}"

systemctl is-enabled "${service_name}"
systemctl is-active "${service_name}"
