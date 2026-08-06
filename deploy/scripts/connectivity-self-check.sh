#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
connectivity_script="${script_dir}/check-connectivity.sh"

grep -Fq 'mqtt_port="${LOGISTICS_MQTT_PORT:-8883}"' "${connectivity_script}"
grep -Fq 'check_tcp "${mqtt_host}" "${mqtt_port}" "MQTT"' "${connectivity_script}"

echo 'Connectivity configuration self-check passed.'
