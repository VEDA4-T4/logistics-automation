# MQTT Security Network Integration Design

## Goal

Make PR #72 deployable on separate Central Server, Control Center, and Device Node hosts without weakening MQTT authentication or breaking HTTP image transfer, while remaining compatible with `feature/rtsp-relay`.

## Scope

- Generate authenticated MQTT runtime settings from the existing central, input, and vision setup scripts.
- Keep credentials and certificates outside tracked configuration.
- Make example MQTT and HTTP endpoints internally consistent for separate hosts.
- Update the deployment guides to describe the implemented TLS support.
- Preserve the RTSP relay branch's `[rtsp]` configuration and secret-handling model during later integration.

The RTSP relay implementation, Control Center layout, and video worker changes are not merged into PR #72.

## Configuration Contract

The three setup scripts consume the same MQTT environment variables:

| Variable | Meaning | Default |
| --- | --- | --- |
| `LOGISTICS_MQTT_HOST` | Broker hostname or IP present in the server certificate SAN | Central host, or `127.0.0.1` for the central script |
| `LOGISTICS_MQTT_PORT` | Broker listener | `8883` |
| `LOGISTICS_MQTT_USERNAME` | Mosquitto password-file user | Component client ID |
| `LOGISTICS_MQTT_PASSWORD` | Password-file secret | None; required when a config is created |
| `LOGISTICS_MQTT_TLS_ENABLED` | `true` or `false` | `true` |
| `LOGISTICS_MQTT_CA_CERTIFICATE` | Client-side CA file | `/etc/logistics/tls/ca.crt` |

Validation only blocks creation or forced replacement of a runtime INI. Existing runtime INIs remain untouched unless `LOGISTICS_FORCE_CONFIG=1`, preserving the current idempotent setup behavior.

When TLS is enabled, the CA path must be non-empty. Authentication is always required by the documented Mosquitto configuration, including the temporary plaintext listener. The plaintext listener exists only for staged transport migration; it does not permit anonymous access.

## Secret Handling

Tracked examples contain descriptive placeholders, never working passwords. The scripts receive secrets through environment variables. Documentation follows the RTSP relay pattern: use a non-echoing Bash prompt inside a subshell, export only for setup, and clear the variables with `trap` on normal exit and signals.

Generated runtime configuration is written through a temporary file and installed with mode `0600`. Broker password files, ACLs, certificates, and private keys remain under `/etc/mosquitto` or `/etc/logistics` with restricted ownership.

## Cross-Host Addressing

MQTT, HTTP, and RTSP addresses are independent:

- MQTT clients use the broker hostname/IP and port 8883.
- Vision uploads use the Central Server HTTP base address, never the Vision host loopback address.
- Control Center image downloads use the Central Server HTTP base address, never the Windows host loopback address.
- RTSP channels continue to use the MediaMTX relay address from `feature/rtsp-relay` after that branch is integrated.

Documentation examples use reserved or descriptive addresses and explicitly require replacement. No team LAN address is committed as a default.

## RTSP Branch Compatibility

`feature/rtsp-relay` and `feature/mqtt-security` both modify `control-center/config/control-centor.ini.example` and `control-center/src/main_window.cpp`. The intended combined result keeps:

- MQTT username, password, `tls_enabled`, and `ca_certificate` fields.
- Central Server HTTP `image_base_url`.
- MediaMTX channel URLs and RTSP worker configuration.
- MQTT TLS construction in `MainWindow`.
- RTSP worker and UI changes from the relay branch.

The MQTT PR does not cherry-pick RTSP UI commits. After either branch reaches `main`, the other branch is updated from `main` and the combined Control Center build and tests are run.

## Failure Behavior

- Missing MQTT password while creating a config: setup exits before modifying the existing file.
- Invalid MQTT port or TLS boolean: setup exits with a precise error.
- TLS enabled with no CA path: setup exits before writing the config.
- Invalid or unreadable CA at runtime: existing C++/Qt clients reject startup or report a connection error.
- Existing config without force: setup preserves it and does not require the new secret environment variables.

## Verification

- Bash syntax and script self-checks cover defaults, invalid values, secret requirements, and rendered MQTT fields.
- Existing central and Device Node MQTT tests cover C++ parsing and TLS CA requirements.
- Control Center is formatted and built when Qt 6.10 is available.
- `git merge-tree` checks future compatibility with `feature/rtsp-relay`.
- Hardware smoke testing covers authenticated TLS MQTT, Vision HTTP upload, Control Center image download, and concurrent RTSP playback.
