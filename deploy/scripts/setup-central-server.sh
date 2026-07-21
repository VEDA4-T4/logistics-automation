#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../.." && pwd)"
build_dir="${LOGISTICS_BUILD_DIR:-${repo_root}/build-central}"
runtime_dir="${LOGISTICS_RUNTIME_DIR:-${repo_root}/runtime/central-server}"
config_path="${LOGISTICS_CONFIG_PATH:-${runtime_dir}/server.ini}"
mqtt_host="${LOGISTICS_MQTT_HOST:-127.0.0.1}"
upload_token="${LOGISTICS_UPLOAD_TOKEN:-}"
force_config="${LOGISTICS_FORCE_CONFIG:-0}"
allow_anonymous_mqtt="${LOGISTICS_ALLOW_ANONYMOUS_MQTT:-0}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This script must run on the central Linux/Raspberry Pi host." >&2
    exit 2
fi
if [[ -z "${upload_token}" ]]; then
    echo "LOGISTICS_UPLOAD_TOKEN must be set." >&2
    exit 2
fi

sudo_command=()
if [[ "${EUID}" -ne 0 ]]; then
    sudo_command=(sudo)
fi

"${sudo_command[@]}" apt-get update
"${sudo_command[@]}" apt-get install -y \
    build-essential cmake ninja-build pkg-config \
    mosquitto mosquitto-clients libmosquitto-dev \
    nlohmann-json3-dev libsqlite3-dev libssl-dev libmicrohttpd-dev

install -d -m 0750 "${runtime_dir}" "${runtime_dir}/images" "${runtime_dir}/logs" \
    "${runtime_dir}/uploads/images" "${runtime_dir}/uploads/logs" "$(dirname -- "${config_path}")"

if [[ "${allow_anonymous_mqtt}" == "1" ]]; then
    temporary_mosquitto_config="$(mktemp)"
    trap 'rm -f -- "${temporary_config:-}" "${temporary_mosquitto_config:-}"' EXIT
    cat >"${temporary_mosquitto_config}" <<'EOF'
listener 1883 0.0.0.0
allow_anonymous true
EOF
    "${sudo_command[@]}" install -m 0644 "${temporary_mosquitto_config}" \
        /etc/mosquitto/conf.d/logistics-integration.conf
    "${sudo_command[@]}" systemctl enable --now mosquitto
    "${sudo_command[@]}" systemctl restart mosquitto
    echo "Enabled anonymous MQTT for integration testing. Do not use this setting in production."
else
    "${sudo_command[@]}" systemctl enable --now mosquitto
    echo "Mosquitto remote authentication was not modified. Configure a listener and credentials before connecting devices."
fi

if [[ -e "${config_path}" && "${force_config}" != "1" ]]; then
    echo "Keeping existing config: ${config_path}"
else
    temporary_config="$(mktemp)"
    trap 'rm -f -- "${temporary_config:-}" "${temporary_mosquitto_config:-}"' EXIT
    cat >"${temporary_config}" <<EOF
[mqtt]
host=${mqtt_host}
port=1883
client_id=central-server
username=
password=
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=true

[device_registry]
path=${runtime_dir}/devices.json

[database]
path=${runtime_dir}/logistics.db
migration_dir=${repo_root}/central-server-rpi/db/migrations
busy_timeout_ms=5000

[storage]
image_root=${runtime_dir}/images
log_root=${runtime_dir}/logs
cleanup_interval_hours=24
mqtt_retention_days=30
device_status_retention_days=30
error_retention_days=180
security_retention_days=180
image_retention_days=30

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
EOF
    install -m 0600 "${temporary_config}" "${config_path}"
    echo "Created config: ${config_path}"
fi

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=ON \
    -DLOGISTICS_BUILD_DEVICE_NODES=OFF \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON
cmake --build "${build_dir}"
ctest --test-dir "${build_dir}" --output-on-failure

echo
echo "Central server setup complete."
echo "Run: ${build_dir}/central-server-rpi/logistics_central_server --config ${config_path}"
echo "Allow TCP ports 1883 (MQTT broker) and 8080 (HTTP upload) from the integration network."
