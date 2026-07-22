#!/usr/bin/env bash

set -euo pipefail

central_host="${LOGISTICS_CENTRAL_HOST:-}"
mqtt_host="${LOGISTICS_MQTT_HOST:-${central_host}}"

if [[ -z "${central_host}" ]]; then
    echo "LOGISTICS_CENTRAL_HOST must be set." >&2
    exit 2
fi

check_tcp() {
    local host="$1"
    local port="$2"
    local label="$3"
    if timeout 3 bash -c "</dev/tcp/${host}/${port}" 2>/dev/null; then
        echo "[OK] ${label}: ${host}:${port}"
    else
        echo "[FAIL] ${label}: ${host}:${port}" >&2
        return 1
    fi
}

check_tcp "${mqtt_host}" 1883 "MQTT"
check_tcp "${central_host}" 8080 "HTTP upload"

http_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
    "http://${central_host}:8080/uploads/images/connectivity-check.jpg")"
if [[ "${http_status}" != "404" ]]; then
    echo "[FAIL] HTTP upload server returned ${http_status}; expected 404 for the probe path." >&2
    exit 1
fi
echo "[OK] HTTP upload server responded with the expected JSON 404."
