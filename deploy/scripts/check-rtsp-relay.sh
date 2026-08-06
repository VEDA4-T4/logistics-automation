#!/usr/bin/env bash
set -euo pipefail

service=logistics-rtsp-relay.service
api_base=http://127.0.0.1:9997/v3/paths/get
curl_options=(--connect-timeout 2 --max-time 5 -fsS)

response_is_ready() {
    # ponytail: the pinned API emits compact JSON; use a JSON parser if the API shape or health rules grow.
    grep -Eq '"ready"[[:space:]]*:[[:space:]]*true' <<<"$1"
}

fetch_path() {
    curl "${curl_options[@]}" "${api_base}/$1" 2>/dev/null || true
}

run_self_check() {
    response_is_ready '{"ready":true}'
    response_is_ready '{ "ready" : true, "tracks": [] }'
    ! response_is_ready '{"ready":false}'
    ! response_is_ready '{}'
    curl() { printf '%s\n' "$@"; }
    mapfile -t request_arguments < <(fetch_path channel1)
    test "${request_arguments[*]}" = "--connect-timeout 2 --max-time 5 -fsS ${api_base}/channel1"
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
    response="$(fetch_path "${channel}")"
    if ! response_is_ready "${response}"; then
        failed+=("${channel}")
    fi
done

if ((${#failed[@]})); then
    printf 'RTSP relay paths not ready: %s\n' "${failed[*]}" >&2
    exit 1
fi

echo "RTSP relay ready: channel1 channel2 channel3 channel4"
