#!/usr/bin/env bash

set -euo pipefail

mediamtx_version=1.19.3
asset_name="mediamtx_v${mediamtx_version}_linux_arm64.tar.gz"
release_base="https://github.com/bluenviron/mediamtx/releases/download/v${mediamtx_version}"
force_config="${LOGISTICS_FORCE_CONFIG:-0}"
unit_source="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../systemd" && pwd)/logistics-rtsp-relay.service"

reject_line_breaks() {
    [[ "$1" != *$'\n'* && "$1" != *$'\r'* ]]
}

validate_rtsp_url() {
    local value=$1
    # ponytail: this accepts the RTSP URL forms used by the cameras; replace it with a URI parser if non-RTSP schemes or exotic authority syntax become requirements.
    reject_line_breaks "${value}" &&
        [[ "${value}" =~ ^rtsps?://([^/@[:space:]]+(:[^@[:space:]]*)?@)?(\[[0-9A-Fa-f:]+\]|[^/:@[:space:]]+)(:[0-9]+)?(/.*)?$ ]]
}

yaml_quote() {
    local value=$1
    reject_line_breaks "${value}" || return 1
    value=${value//\'/\'\'}
    printf "'%s'" "${value}"
}

validate_relay_credentials() {
    local user=$1 password=$2
    [[ -n "${user}" && -n "${password}" && "${user}" != any ]] &&
        reject_line_breaks "${user}" && reject_line_breaks "${password}"
}

render_config() {
    local source_1=$1 source_2=$2 source_3=$3 source_4=$4 relay_user=$5 relay_password=$6

    printf '%s\n' \
        'logLevel: info' \
        'logDestinations: [stdout]' \
        '' \
        'authMethod: internal' \
        'authInternalUsers:' \
        "  - user: $(yaml_quote "${relay_user}")" \
        "    pass: $(yaml_quote "${relay_password}")" \
        '    ips: []' \
        '    permissions:' \
        '      - action: read' \
        '        path: ~^channel[1-4]$' \
        '  - user: any' \
        '    pass:' \
        '    ips: [127.0.0.1, ::1]' \
        '    permissions:' \
        '      - action: api' \
        '' \
        'api: true' \
        'apiAddress: 127.0.0.1:9997' \
        'metrics: false' \
        'pprof: false' \
        'playback: false' \
        '' \
        'rtsp: true' \
        'rtspTransports: [tcp]' \
        'rtspAddress: :8554' \
        'rtspAuthMethods: [digest]' \
        'rtmp: false' \
        'hls: false' \
        'webrtc: false' \
        'srt: false' \
        'moq: false' \
        '' \
        'paths:' \
        '  channel1:' \
        "    source: $(yaml_quote "${source_1}")" \
        '    sourceOnDemand: false' \
        '    rtspTransport: tcp' \
        '  channel2:' \
        "    source: $(yaml_quote "${source_2}")" \
        '    sourceOnDemand: false' \
        '    rtspTransport: tcp' \
        '  channel3:' \
        "    source: $(yaml_quote "${source_3}")" \
        '    sourceOnDemand: false' \
        '    rtspTransport: tcp' \
        '  channel4:' \
        "    source: $(yaml_quote "${source_4}")" \
        '    sourceOnDemand: false' \
        '    rtspTransport: tcp'
}

should_keep_config() {
    [[ "${force_config}" != 1 ]] &&
        "${sudo_command[@]}" test -e "${config_path}"
}

run_self_check() {
    test "${mediamtx_version}" = 1.19.3
    test "${asset_name}" = mediamtx_v1.19.3_linux_arm64.tar.gz
    validate_rtsp_url 'rtsp://camera:554/stream'
    validate_rtsp_url 'rtsps://user:p%21@camera.example/stream'
    ! validate_rtsp_url 'http://camera/stream'
    ! validate_rtsp_url $'rtsp://camera/stream\ninvalid'
    test "$(yaml_quote "a'b")" = "'a''b'"
    validate_relay_credentials 'control-center' 'relay-password'
    ! validate_relay_credentials 'any' 'relay-password'
    config_path="${BASH_SOURCE[0]}"
    force_config=0
    sudo_command=()
    should_keep_config
    force_config=1
    ! should_keep_config
    sudo_command=(command)
    force_config=0
    should_keep_config
    force_config=1
    ! should_keep_config
    expected_directory_install='install -d -m 0750 -o root -g logistics /etc/logistics'
    grep -Fq "\"\${sudo_command[@]}\" ${expected_directory_install}" "${BASH_SOURCE[0]}"

    rendered="$(render_config \
        'rtsp://camera-1/stream' 'rtsp://camera-2/stream' \
        'rtsp://camera-3/stream' 'rtsp://camera-4/stream' \
        'control-center' 'relay-password')"
    test "$(grep -c 'sourceOnDemand: false' <<<"${rendered}")" -eq 4
    test "$(grep -c 'rtspTransport: tcp' <<<"${rendered}")" -eq 4
    grep -Fq 'path: ~^channel[1-4]$' <<<"${rendered}"
    ! grep -Fq 'sourceOnDemand: true' <<<"${rendered}"
    echo 'RTSP relay setup self-check passed.'
}

if [[ "${1:-}" == "--self-check" ]]; then
    run_self_check
    exit 0
fi

source_1="${LOGISTICS_RTSP_SOURCE_1:-}"
source_2="${LOGISTICS_RTSP_SOURCE_2:-}"
source_3="${LOGISTICS_RTSP_SOURCE_3:-}"
source_4="${LOGISTICS_RTSP_SOURCE_4:-}"
relay_user="${LOGISTICS_RTSP_RELAY_USER:-}"
relay_password="${LOGISTICS_RTSP_RELAY_PASSWORD:-}"
binary_path=/usr/local/bin/mediamtx
config_path=/etc/logistics/rtsp-relay.yml

if [[ "$(uname -s)" != Linux ]]; then
    echo 'This script must run on a Linux host.' >&2
    exit 2
fi
if [[ "$(uname -m)" != aarch64 && "$(uname -m)" != arm64 ]]; then
    echo 'This script requires an ARM64 host.' >&2
    exit 2
fi
for command in curl tar sha256sum install; do
    command -v "${command}" >/dev/null || {
        echo "Required command not found: ${command}" >&2
        exit 2
    }
done
id logistics >/dev/null 2>&1 || {
    echo 'The logistics user must exist.' >&2
    exit 2
}
getent group logistics >/dev/null || {
    echo 'The logistics group must exist.' >&2
    exit 2
}
for source in "${source_1}" "${source_2}" "${source_3}" "${source_4}"; do
    validate_rtsp_url "${source}" || {
        echo 'Each LOGISTICS_RTSP_SOURCE value must be a valid RTSP or RTSPS URL.' >&2
        exit 2
    }
done
if ! validate_relay_credentials "${relay_user}" "${relay_password}"; then
    echo "LOGISTICS_RTSP_RELAY_USER and LOGISTICS_RTSP_RELAY_PASSWORD must be non-empty single-line values, and the user must not be 'any'." >&2
    exit 2
fi
if [[ "${force_config}" != 0 && "${force_config}" != 1 ]]; then
    echo 'LOGISTICS_FORCE_CONFIG must be 0 or 1.' >&2
    exit 2
fi

sudo_command=()
if [[ "${EUID}" -ne 0 ]]; then
    command -v sudo >/dev/null || {
        echo 'sudo is required when not running as root.' >&2
        exit 2
    }
    sudo_command=(sudo)
fi

download_dir="$(mktemp -d)"
temporary_config=
trap 'rm -rf -- "${download_dir:-}" "${temporary_config:-}"' EXIT

curl -fsSL "${release_base}/${asset_name}" --output "${download_dir}/${asset_name}"
curl -fsSL "${release_base}/checksums.sha256" --output "${download_dir}/checksums.sha256"
(
    cd "${download_dir}"
    grep -F "  ${asset_name}" checksums.sha256 >selected.sha256
    test "$(wc -l <selected.sha256)" -eq 1
    sha256sum --check selected.sha256
    tar -xzf "${asset_name}" mediamtx
    test -x mediamtx
)

"${sudo_command[@]}" install -m 0755 "${download_dir}/mediamtx" "${binary_path}.new"
"${sudo_command[@]}" mv -f -- "${binary_path}.new" "${binary_path}"

if should_keep_config; then
    echo 'Keeping existing RTSP relay config'
else
    temporary_config="$(mktemp)"
    render_config "${source_1}" "${source_2}" "${source_3}" "${source_4}" \
        "${relay_user}" "${relay_password}" >"${temporary_config}"
    "${sudo_command[@]}" install -d -m 0750 -o root -g logistics /etc/logistics
    "${sudo_command[@]}" install -m 0640 -o root -g logistics "${temporary_config}" "${config_path}.new"
    "${sudo_command[@]}" mv -f -- "${config_path}.new" "${config_path}"
fi

"${sudo_command[@]}" install -m 0644 "${unit_source}" /etc/systemd/system/logistics-rtsp-relay.service
"${sudo_command[@]}" systemctl daemon-reload
"${sudo_command[@]}" systemctl enable --now logistics-rtsp-relay.service
