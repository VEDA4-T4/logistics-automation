#!/usr/bin/env bash
set -euo pipefail

service=logistics-rtsp-relay.service
api_base=http://127.0.0.1:9997/v3/paths/get

response_is_ready() {
    # ponytail: the pinned API emits compact JSON; use a JSON parser if the API shape or health rules grow.
    grep -Eq '"ready"[[:space:]]*:[[:space:]]*true' <<<"$1"
}

run_self_check() {
    response_is_ready '{"ready":true}'
    response_is_ready '{ "ready" : true, "tracks": [] }'
    ! response_is_ready '{"ready":false}'
    ! response_is_ready '{}'
    echo 'RTSP relay readiness self-check passed.'
}

if [[ "${1:-}" == "--self-check" ]]; then
    run_self_check
    exit 0
fi

if ! systemctl is-active --quiet "${service}"; then
    echo "RTSP relay service is not active." >&2
    exit 1
fi

failed=()
for channel in channel1 channel2 channel3 channel4; do
    response="$(curl -fsS "${api_base}/${channel}" 2>/dev/null || true)"
    if ! response_is_ready "${response}"; then
        failed+=("${channel}")
    fi
done

if ((${#failed[@]})); then
    printf 'RTSP relay paths not ready: %s\n' "${failed[*]}" >&2
    exit 1
fi

echo "RTSP relay ready: channel1 channel2 channel3 channel4"
