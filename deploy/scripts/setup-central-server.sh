#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
build_dir="${LOGISTICS_BUILD_DIR:-${repo_root}/build-central}"
install_prefix="/opt/logistics-automation"
runtime_dir="/var/lib/logistics"
config_path="/etc/logistics/server.ini"
service_name="logistics-central-server.service"
mqtt_host="${LOGISTICS_MQTT_HOST:-127.0.0.1}"
mqtt_port="${LOGISTICS_MQTT_PORT:-8883}"
mqtt_username="${LOGISTICS_MQTT_USERNAME:-central-server}"
mqtt_password="${LOGISTICS_MQTT_PASSWORD:-}"
mqtt_tls_enabled="${LOGISTICS_MQTT_TLS_ENABLED:-true}"
mqtt_ca_certificate="${LOGISTICS_MQTT_CA_CERTIFICATE:-/etc/logistics/tls/ca.crt}"
upload_token="${LOGISTICS_UPLOAD_TOKEN:-}"
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

should_write_config() {
    local config_exists=$1
    local force_write=$2

    [[ "${config_exists}" == "0" || "${force_write}" == "1" ]]
}

run_self_check() {
    validate_mqtt_settings mqtt.example 8883 central-server secret true /etc/logistics/tls/ca.crt
    validate_mqtt_settings mqtt.example 1883 central-server secret false ""
    ! validate_mqtt_settings mqtt.example 0 central-server secret true /etc/logistics/tls/ca.crt 2>/dev/null
    ! validate_mqtt_settings mqtt.example 8883 central-server "" true /etc/logistics/tls/ca.crt 2>/dev/null
    ! validate_mqtt_settings mqtt.example 8883 central-server secret yes /etc/logistics/tls/ca.crt 2>/dev/null
    ! validate_mqtt_settings mqtt.example 8883 central-server secret true "" 2>/dev/null
    ! validate_mqtt_settings $'mqtt.example\ninvalid' 8883 central-server secret true /etc/logistics/tls/ca.crt 2>/dev/null
    should_write_config 0 0
    ! should_write_config 1 0
    should_write_config 1 1
    echo "$(basename -- "${BASH_SOURCE[0]}") self-check passed."
}

if [[ "${1:-}" == "--self-check" ]]; then
    run_self_check
    exit 0
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must run on the central Linux/Raspberry Pi host." >&2
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
if should_write_config "${config_exists}" "${force_config}"; then
    write_config=1
fi

if [[ "${write_config}" == "1" && -z "${upload_token}" ]]; then
    echo "LOGISTICS_UPLOAD_TOKEN must be set." >&2
    exit 2
fi

if [[ "${write_config}" == "1" ]]; then
    validate_mqtt_settings \
        "${mqtt_host}" \
        "${mqtt_port}" \
        "${mqtt_username}" \
        "${mqtt_password}" \
        "${mqtt_tls_enabled}" \
        "${mqtt_ca_certificate}" || exit 2
fi

if [[ "${install_dependencies}" == "1" ]]; then
    "${sudo_command[@]}" apt-get update
    "${sudo_command[@]}" apt-get install -y \
        build-essential cmake ninja-build pkg-config \
        libmosquitto-dev nlohmann-json3-dev libsqlite3-dev libssl-dev libmicrohttpd-dev
fi

if ! getent group logistics >/dev/null; then
    "${sudo_command[@]}" groupadd --system logistics
fi

if ! getent passwd logistics >/dev/null; then
    "${sudo_command[@]}" useradd \
        --system \
        --gid logistics \
        --home-dir "${runtime_dir}" \
        --shell /usr/sbin/nologin \
        logistics
fi

"${sudo_command[@]}" install \
    -d \
    -o root \
    -g logistics \
    -m 0750 \
    "$(dirname -- "${config_path}")"

"${sudo_command[@]}" install \
    -d \
    -o logistics \
    -g logistics \
    -m 0750 \
    "${runtime_dir}" \
    "${runtime_dir}/images" \
    "${runtime_dir}/logs" \
    "${runtime_dir}/uploads" \
    "${runtime_dir}/uploads/images" \
    "${runtime_dir}/uploads/logs"

if [[ "${write_config}" != "1" ]]; then
    echo "Keeping existing config: ${config_path}"
else
    temporary_config="$(mktemp)"
    trap 'rm -f -- "${temporary_config:-}"' EXIT
    cat >"${temporary_config}" <<EOF
[mqtt]
host=${mqtt_host}
port=${mqtt_port}
client_id=central-server
username=${mqtt_username}
password=${mqtt_password}
tls_enabled=${mqtt_tls_enabled}
ca_certificate=${mqtt_ca_certificate}
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=false

[device_registry]
path=${runtime_dir}/devices.json

[database]
path=${runtime_dir}/logistics.db
migration_dir=${install_prefix}/share/logistics/migrations
busy_timeout_ms=5000

[storage]
image_root=${runtime_dir}/images
cleanup_interval_hours=24
mqtt_retention_days=30
device_status_retention_days=30
error_retention_days=180
security_retention_days=180
image_retention_days=30
upload_retention_days=30

[http]
enabled=true
port=8080
tls_enabled=false
tls_certificate=/etc/logistics/tls/server.crt
tls_private_key=/etc/logistics/tls/server.key
bearer_token=${upload_token}
upload_root=${runtime_dir}/uploads

[routing]
qt_client_id=control-center

[process]
enabled=true
server_id=central-server
input_device_id=PI-INPUT-01
vision_device_id=PI-VISION-01
gripper_device_id=PI-GRIPPER-01
sorting_device_id=PI-SORTING-01
line_tracer_device_id=PI-LT-01
line_tracer_enabled=true
line_tracer_initial_position=A
default_destination=3
EOF
    "${sudo_command[@]}" install \
        -o root \
        -g logistics \
        -m 0640 \
        "${temporary_config}" \
        "${config_path}"
    echo "Created config: ${config_path}"
fi

"${sudo_command[@]}" chown root:logistics "${config_path}"
"${sudo_command[@]}" chmod 0640 "${config_path}"

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=ON \
    -DLOGISTICS_BUILD_DEVICE_NODES=OFF \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON
cmake --build "${build_dir}"
ctest --test-dir "${build_dir}" --output-on-failure

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

if ! "${sudo_command[@]}" systemctl restart "${service_name}"; then
    "${sudo_command[@]}" systemctl status "${service_name}" --no-pager || true
    exit 5
fi

if ! "${sudo_command[@]}" systemctl is-active --quiet "${service_name}"; then
    "${sudo_command[@]}" systemctl status "${service_name}" --no-pager || true
    exit 5
fi

echo
echo "Central server setup complete."
echo "Installed: ${install_prefix}/bin/logistics_central_server"
echo "Config: ${config_path}"
echo "Service: ${service_name}"
echo "This script did not modify or restart Mosquitto."
