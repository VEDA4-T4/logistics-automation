# MQTT Security Network Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make authenticated MQTT TLS setup work across separate hosts while preserving HTTP image routing and future RTSP relay integration.

**Architecture:** Extend the three existing setup scripts instead of introducing a new configuration framework. Each script validates the same MQTT environment contract only when creating a runtime INI, renders its existing INI atomically, and keeps component-specific HTTP/UART fields. Tracked examples and deployment documentation use placeholders and distinct MQTT, HTTP, and RTSP endpoints.

**Tech Stack:** Bash, CMake/CTest, C++20, libmosquitto, Qt 6.10, Mosquitto ACL/TLS configuration.

## Global Constraints

- Work on `feature/mqtt-security`, not `main`.
- Do not merge the RTSP UI branch into PR #72.
- Do not commit passwords, upload tokens, certificates, private keys, or environment-specific LAN addresses.
- Preserve existing runtime INIs unless `LOGISTICS_FORCE_CONFIG=1`.
- Keep Mosquitto anonymous access disabled on both 1883 and 8883.
- Use existing dependencies and script patterns; add no dependency or general-purpose abstraction.

---

### Task 1: Secure setup-script regression checks

**Files:**
- Modify: `deploy/scripts/setup-central-server.sh`
- Modify: `deploy/scripts/setup-input-node.sh`
- Modify: `deploy/scripts/setup-vision-node.sh`

**Interfaces:**
- Consumes: existing `LOGISTICS_FORCE_CONFIG` and component identity variables.
- Produces: `--self-check` entry points that exercise MQTT validation without root access, builds, downloads, or file installation.

- [ ] Add self-check assertions requiring a password for new authenticated configurations, accepting ports 1-65535, accepting only `true`/`false`, and requiring a CA path when TLS is true.
- [ ] Run each `--self-check` and confirm it fails because the validation functions do not exist yet.
- [ ] Add the minimum validation and MQTT rendering functions inside each existing script.
- [ ] Run all three self-checks and `bash -n` until they pass.
- [ ] Commit scripts and their checks as `fix: secure generated MQTT runtime configs`.

### Task 2: Cross-host configuration examples

**Files:**
- Modify: `central-server-rpi/config/server.ini.example`
- Modify: `control-center/config/control-centor.ini.example`
- Modify: `device-rpi/config/node.ini.example`
- Modify: `device-rpi/config/sorting-node.ini.example`

**Interfaces:**
- Consumes: MQTT configuration keys already parsed by Central Server, Device Node, and Control Center.
- Produces: copy-safe examples with explicit placeholders and separate broker, Central HTTP, and RTSP endpoints.

- [ ] Add a repository check that rejects `test-for-test` and detects loopback HTTP endpoints in the remote-host Vision and Control Center examples.
- [ ] Run the check and confirm it fails on the current examples.
- [ ] Replace known passwords and environment-specific addresses with descriptive placeholders or documentation-only reserved addresses.
- [ ] Set Vision upload and Control Center image URLs to the documented Central Server address while leaving `[rtsp]` keys intact for future relay integration.
- [ ] Run the repository check and configuration parser tests.
- [ ] Commit as `fix: align cross-device network endpoints`.

### Task 3: Deployment documentation

**Files:**
- Modify: `README.md`
- Modify: `deploy/mosquitto/README.md`
- Modify: `deploy/scripts/README.md`
- Modify: `docs/guides/integration-runbook.md`
- Modify: `docs/guides/operations-troubleshooting.md`

**Interfaces:**
- Consumes: the environment contract and deployment behavior from Tasks 1-2.
- Produces: one consistent operator flow for password creation, CA installation, runtime config generation, and cross-host smoke tests.

- [ ] Remove statements that MQTT TLS is unimplemented or that clients must not use 8883.
- [ ] Document non-echoing password input and cleanup using the RTSP relay subshell/trap pattern.
- [ ] Document that 1883 remains authenticated during migration and that the broker certificate SAN must match the configured MQTT host.
- [ ] Document distinct MQTT, HTTP upload/download, and RTSP relay endpoints.
- [ ] Add commands for anonymous rejection, bad-password rejection, TLS connection, image upload/download, and later RTSP concurrency checks.
- [ ] Scan the documentation for contradictory TLS status and committed secret examples.
- [ ] Commit as `docs: align MQTT TLS and RTSP deployment`.

### Task 4: Full verification and RTSP merge preview

**Files:**
- Verify only; no planned production changes.

**Interfaces:**
- Consumes: all previous task outputs and both feature branch tips.
- Produces: fresh verification evidence and a list of any remaining hardware-only checks.

- [ ] Run `git diff --check`.
- [ ] Run the three setup-script self-checks and `bash -n` on every deployment script.
- [ ] Build and run Central Server and Device Node tests with the available CMake build.
- [ ] Run clang-format verification on modified C++ files; if no C++ file changed, record that it is not applicable.
- [ ] Build and test Control Center when Qt 6.10 is available; otherwise report the missing dependency explicitly.
- [ ] Run `git merge-tree` against `feature/rtsp-relay` and verify the intended combined `[mqtt]`, `[http]`, and `[rtsp]` settings.
- [ ] Review `git status`, commit only scoped files, and leave `.superpowers/` and user files untouched.
