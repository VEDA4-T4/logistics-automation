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

echo 'MQTT setup security self-checks passed.'
