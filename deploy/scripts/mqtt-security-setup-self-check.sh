#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

for setup_script in setup-central-server.sh setup-input-node.sh setup-vision-node.sh; do
    if output="$("${BASH}" "${script_dir}/${setup_script}" --self-check 2>&1)"; then
        status=0
    else
        status=$?
    fi
    expected="${setup_script} MQTT security self-check passed."
    if ((status != 0)) || [[ "${output}" != *"${expected}"* ]]; then
        printf '%s\n' "${output}" >&2
        echo "Self-check failed for ${setup_script} with status ${status}; expected: ${expected}" >&2
        exit 1
    fi
done

examples=(
    "${script_dir}/../../central-server-rpi/config/server.ini.example"
    "${script_dir}/../../control-center/config/control-centor.ini.example"
    "${script_dir}/../../device-rpi/config/node.ini.example"
    "${script_dir}/../../device-rpi/config/sorting-node.ini.example"
)

if grep -Fq 'test-for-test' "${examples[@]}"; then
    echo 'Tracked configuration examples must not contain a working MQTT password.' >&2
    exit 1
fi

grep -Fqx 'host=mqtt.logistics.local' "${examples[@]}"
grep -Fqx 'image_base_url=http://central-server.logistics.local:8080/' \
    "${script_dir}/../../control-center/config/control-centor.ini.example"
grep -Fqx 'endpoint_url=http://central-server.logistics.local:8080/api/v1/uploads/images' \
    "${script_dir}/../../device-rpi/config/node.ini.example"

echo 'MQTT setup security self-checks passed.'
