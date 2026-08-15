# 공정 워크플로 하드웨어 검증

## 상태

**NOT YET physically executed.** 이 문서는 실제 장비 시험의 실행 기록과 배포 판정을 위한 절차다. 아래 표의
결과와 증거는 시험을 수행한 뒤에만 채운다.

## 사전 조건

- 중앙 서버는 `startup_mode=fresh`로 시작한다. fresh 시작 뒤에는 `product_catalog`만 보존하고 이전 작업, 명령,
  inbox/outbox, 처리 메시지는 남아 있지 않아야 한다. 새 세션의 `STOPPED` 공정 상태와 새 epoch는 생성될 수 있다.
- 중앙 서버, input, vision, gripper, sorting 장비 ID와 TLS/ACL 연결을 확인한다. 이 문서와 셸 이력에는
  비밀번호, 토큰, 개인키를 기록하지 않는다.
- 시험마다 새 `RUN_ID`와 중앙이 만든 새 process epoch를 기록한다. `RUN_ID`를 생성·검증하고 run 절대 경로를
  확인한 뒤에만 input/sorting/vision/gripper spool을 격리한다. 기존 spool 또는 shared root는 삭제하지 않는다.

  ```sh
  set -eu
  RUN_ID="${RUN_ID:-run-$(date -u +%Y%m%dT%H%M%SZ)}"
  case "${RUN_ID}" in
    run-?*) ;;
    *) printf '%s\n' 'RUN_ID must begin with run-' >&2; exit 1 ;;
  esac
  case "${RUN_ID}" in
    *[!A-Za-z0-9._-]*|*..*|.|..) printf '%s\n' 'unsafe RUN_ID' >&2; exit 1 ;;
  esac
  SPOOL_BASE=/var/lib/logistics/mqtt-spool
  SPOOL_BASE_REAL="$(sudo readlink -f -- "${SPOOL_BASE}")"
  RUN_SPOOL="${SPOOL_BASE_REAL}/${RUN_ID}"
  case "${RUN_SPOOL}" in
    "${SPOOL_BASE_REAL}"/run-*) ;;
    *) printf '%s\n' 'unsafe spool path' >&2; exit 1 ;;
  esac
  if [ -e "${RUN_SPOOL}" ]; then
    printf '%s\n' "run spool already exists: ${RUN_SPOOL}" >&2; exit 1
  fi
  sudo install -d -o root -g root -m 0750 "${RUN_SPOOL}"
  test "$(sudo readlink -f -- "${RUN_SPOOL}")" = "${RUN_SPOOL}"

  # Each host keeps non-secret run metadata and captures under its own absolute evidence directory.
  EVIDENCE_USER="$(id -un)"
  EVIDENCE_GROUP="$(id -gn)"
  EVIDENCE_BASE=/var/lib/logistics/hardware-validation
  sudo install -d -o "${EVIDENCE_USER}" -g "${EVIDENCE_GROUP}" -m 0750 "${EVIDENCE_BASE}"
  EVIDENCE_BASE_REAL="$(sudo readlink -f -- "${EVIDENCE_BASE}")"
  EVIDENCE_DIR="${EVIDENCE_BASE_REAL}/${RUN_ID}"
  test ! -e "${EVIDENCE_DIR}"
  sudo install -d -o "${EVIDENCE_USER}" -g "${EVIDENCE_GROUP}" -m 0750 "${EVIDENCE_DIR}"
  RUN_ENV="${EVIDENCE_DIR}/run.env"
  umask 077
  {
    printf "RUN_ID='%s'\n" "${RUN_ID}"
    printf "SPOOL_BASE_REAL='%s'\n" "${SPOOL_BASE_REAL}"
    printf "RUN_SPOOL='%s'\n" "${RUN_SPOOL}"
    printf "EVIDENCE_DIR='%s'\n" "${EVIDENCE_DIR}"
    printf "RUN_START=''\nRUN_END=''\n"
  } > "${RUN_ENV}"
  chmod 0600 "${RUN_ENV}"
  ```

  공통 MQTT client는 `publish_spool_directory/<device_id>/`와 그 아래 `inbound/`를 만든다. 따라서 INI의
  `publish_spool_directory`는 역할별 경로로 설정하고, 각 정확한 role/device/inbound 경로를 실행 계정이 쓸 수
  있게 미리 만든다. `PI-…` 값은 실제 INI의 `device_id`로 교체한다.

  ```sh
  # Yocto systemd users: logistics-input:logistics, logistics-sorting:logistics
  sudo install -d -o logistics-input -g logistics -m 0750 "${RUN_SPOOL}/input"
  sudo install -d -o logistics-input -g logistics -m 0750 "${RUN_SPOOL}/input/PI-INPUT-01/inbound"
  sudo install -d -o logistics-sorting -g logistics -m 0750 "${RUN_SPOOL}/sorting"
  sudo install -d -o logistics-sorting -g logistics -m 0750 "${RUN_SPOOL}/sorting/PI-SORTING-01/inbound"

  # Foreground vision/gripper: use the account that will run each process.
  VISION_USER="$(id -un)"; VISION_GROUP="$(id -gn)"
  GRIPPER_USER="$(id -un)"; GRIPPER_GROUP="$(id -gn)"
  sudo install -d -o "${VISION_USER}" -g "${VISION_GROUP}" -m 0750 "${RUN_SPOOL}/vision"
  sudo install -d -o "${VISION_USER}" -g "${VISION_GROUP}" -m 0750 "${RUN_SPOOL}/vision/PI-VISION-01/inbound"
  sudo install -d -o "${GRIPPER_USER}" -g "${GRIPPER_GROUP}" -m 0750 "${RUN_SPOOL}/gripper"
  sudo install -d -o "${GRIPPER_USER}" -g "${GRIPPER_GROUP}" -m 0750 "${RUN_SPOOL}/gripper/PI-GRIPPER-01/inbound"
  ```

  Set input, sorting, vision, and gripper `publish_spool_directory` to `${RUN_SPOOL}/input`,
  `${RUN_SPOOL}/sorting`, `${RUN_SPOOL}/vision`, and `${RUN_SPOOL}/gripper` respectively. Confirm those resolved paths
  and ownership before each process starts; isolate only and never remove an earlier run's spool. On every new terminal
  (and every host) use that host's absolute `${EVIDENCE_DIR}/run.env`; it contains no secrets and is mode `0600`.

## 빌드 및 배포 범위

| 변경 범위 | 필요한 작업 | 이번 판정 |
| --- | --- | --- |
| 중앙 서버 | 재빌드, `startup_mode=fresh` 확인 후 재시작 | 적용 여부 기록 |
| shared/Pi common | 영향 받는 Yocto Pi 이미지 재빌드·배포 | input/sorting/vision/gripper별 기록 |
| vision 노드 | vision 바이너리/이미지 재빌드·배포 | 적용 여부 기록 |
| STM32 | 재플래시하지 않음 | UART 계약 불일치 실측 시 별도 작업 |

Yocto input/sorting 이미지의 소스는 `3c87abcb49edecff24e6f676a223b51f81721931`에 고정한다. gripper와 vision의
배포 artifact도 같은 검증 대상 소스와 구성에서 생성했는지 기록한다.

## 실행과 3상자 UTC 나노초 타임라인

1. 중앙과 모든 노드가 안전한 정지/초기 상태인지 확인한 뒤 `START`를 한 번만 보낸다.
2. 실제 상자 세 개를 한 번에 하나씩 통과시킨다. 각 timeline 값은 다음 두 evidence class 중 하나를 반드시
   표시한다: (a) application/MQTT payload의 timestamp와 그 payload가 제공한 precision, 또는 (b) operator가
   `date -u --iso-8601=ns`로 찍은 UTC nanosecond marker. journal 수신 시각은 timeline의 ns 근거가 아니다.
3. ns 값을 주장하는 cell은 `payload-ns` 또는 `operator-marker-ns`를 함께 적는다. 다른 precision의 application
   timestamp는 원문 precision을 보존해 기록하고 ns로 채우거나 보간하지 않는다.
4. 아래 필드가 빠지거나 순서가 어긋나면 즉시 실패로 표시하고 해당 `RUN_ID`에서 추가 상자를 투입하지 않는다.

| box | sensor publish | central receive | input STOP command / ACK | WORK_CREATED / WORK_ASSIGNED | first box / barcode / success | gripper pickup / HOME / terminal response | sorting destination / START / detected / STOP / complete | verdict |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 |  |  |  |  |  |  |  |  |
| 2 |  |  |  |  |  |  |  |  |
| 3 |  |  |  |  |  |  |  |  |

## ESTOP / RECOVERY 매트릭스

각 run에서 아래 여섯 단계마다 한 번씩 ESTOP을 수행한다. 장치가 실제로 정지한 것을 확인한 뒤 RECOVERY를
보내며, RECOVERY 성공은 새 START가 아니라 중앙 `STOPPED`와 모든 노드의 정지/초기 상태를 뜻한다.

| 단계 | ESTOP 시점 | RECOVERY 후 필수 확인 | 결과/증거 |
| --- | --- | --- | --- |
| 1. 투입 감지 전 | START 후 input 동작 중 | active work 및 pending command가 0, input 정지 |  |
| 2. input STOP 대기 | 감지 후 STOP command/ACK 사이 | 이전 STOP 또는 WORK_CREATED 재발행 없음 |  |
| 3. vision 인식 | 위치 또는 barcode 누적 중 | vision 작업/결과/outbox/캡처 상태 초기화 |  |
| 4. gripper 동작 | pickup 또는 HOME 중 | gripper가 HOME과 terminal RECOVERY 응답 뒤 정지 |  |
| 5. sorting 이송 | 목적지 설정 또는 START 뒤 | sorting 정지, 목적지/cycle 상태 초기화 |  |
| 6. sorting 완료/운송 경계 | detected/STOP/complete 또는 line-tracer handoff 중 | 완료·운송 명령이 재개되지 않고 모든 장치 `STOPPED` |  |

각 행에서 RECOVERY 응답 누락 또는 실패가 있으면 중앙은 `RECOVERY` 또는 `ESTOP`을 유지해야 하며, 일부 장치만
성공한 상태를 완료로 기록하지 않는다. 새 START 뒤에는 이전 epoch/work ID가 아닌 새 work ID만 허용한다.

## 증거 수집 명령

장비 경로와 서비스 이름은 설치 환경에 맞게 확인한 뒤 사용한다. 민감한 인증 정보는 별도 보안 절차로 제공하며
명령줄이나 이 문서에 넣지 않는다.

### 모든 새 terminal의 run context source/검증

각 새 terminal은 다음처럼 **기록한 절대 `run.env` 경로를 인용해** source하고 검증한 뒤에만 다음 명령을 실행한다.
`run.env`는 앞 단계가 만든 mode `0600` non-secret metadata 파일이며, `RUN_ID`와 절대 spool/evidence 경로를 새로
추측하지 않는다.

```sh
set -eu
RUN_ENV='/var/lib/logistics/hardware-validation/run-YYYYMMDDTHHMMSSZ/run.env' # recorded absolute path
test -r "${RUN_ENV}"
test "$(stat -c '%a' "${RUN_ENV}")" = 600
. "${RUN_ENV}"
case "${RUN_ID}" in run-?*) ;; *) printf '%s\n' 'unsafe RUN_ID' >&2; exit 1 ;; esac
case "${RUN_ID}" in *[!A-Za-z0-9._-]*|*..*|.|..) printf '%s\n' 'unsafe RUN_ID' >&2; exit 1 ;; esac
SPOOL_BASE_REAL_NOW="$(sudo readlink -f -- /var/lib/logistics/mqtt-spool)"
test "${SPOOL_BASE_REAL}" = "${SPOOL_BASE_REAL_NOW}"
test "${RUN_SPOOL}" = "${SPOOL_BASE_REAL}/${RUN_ID}"
test "$(sudo readlink -f -- "${RUN_SPOOL}")" = "${RUN_SPOOL}"
test "$(readlink -f -- "${EVIDENCE_DIR}")" = "${EVIDENCE_DIR}"
```

### START 직전 UTC journal 범위 기록

`START`를 보내기 직전, 중앙 서버 terminal에서 위 source/검증 block을 실행한 뒤 `RUN_START`를 UTC 초 정밀도로
기록한다. journalctl의 `--since/--until` 형식과 호환되며, timeline의 ns 증거는 여전히 payload 또는 operator marker다.

```sh
RUN_START="$(date --utc '+%Y-%m-%d %H:%M:%S')"
RUN_END=''
umask 077
{
  printf "RUN_ID='%s'\n" "${RUN_ID}"
  printf "SPOOL_BASE_REAL='%s'\nRUN_SPOOL='%s'\n" "${SPOOL_BASE_REAL}" "${RUN_SPOOL}"
  printf "EVIDENCE_DIR='%s'\nRUN_START='%s'\nRUN_END=''\n" "${EVIDENCE_DIR}" "${RUN_START}"
} > "${RUN_ENV}"
chmod 0600 "${RUN_ENV}"
```

### Step A — 첫 fresh 시작 전 catalog 보존 기준 기록

중앙 서버를 처음 `startup_mode=fresh`로 시작하기 전에 중앙 host에서 실행한다. 이전 precondition terminal의
변수에 의존하지 않고, 기록한 validated `RUN_ID`를 다시 명시한다. 같은 `RUN_ID` evidence file이 이미 있으면
기존 증거를 덮어쓰지 않고 중단한다.

```sh
# First source and validate the recorded absolute RUN_ENV using the block above.
CATALOG_EVIDENCE="${EVIDENCE_DIR}/catalog-pre-fresh.txt"
test ! -e "${CATALOG_EVIDENCE}"
sudo sqlite3 /var/lib/logistics/logistics.db 'SELECT count(*) FROM product_catalog;' > "${CATALOG_EVIDENCE}"
test "$(sed -n '$=' "${CATALOG_EVIDENCE}")" -eq 1
```

### Step B — fresh 시작 후 runtime zero와 catalog 비교

```bash
set -e
# First source and validate the recorded absolute RUN_ENV using the block above. Restart/terminal 경계를 넘어
# Step A의 absolute evidence file에서 pre-fresh count를 다시 읽고 검증한다.
CATALOG_EVIDENCE="${EVIDENCE_DIR}/catalog-pre-fresh.txt"
CATALOG_COUNT_BEFORE="$(cat "${CATALOG_EVIDENCE}")"
case "${CATALOG_COUNT_BEFORE}" in *[!0-9]*|'') printf '%s\n' 'invalid pre-fresh catalog evidence' >&2; exit 1 ;; esac
CATALOG_COUNT_AFTER="$(sudo sqlite3 /var/lib/logistics/logistics.db 'SELECT count(*) FROM product_catalog;')"
test "${CATALOG_COUNT_AFTER}" = "${CATALOG_COUNT_BEFORE}"

# 중앙 SQLite: fresh 시작 직후 catalog는 보존, 그 외 과거 runtime/event/history 행은 0이어야 함.
# schema_migrations는 현재 schema metadata이므로 0을 기대하지 않는다. Query output을 evidence file에 보존한다.
FRESH_RUNTIME_EVIDENCE="${EVIDENCE_DIR}/fresh-runtime.txt"
sudo sqlite3 -noheader -separator '|' /var/lib/logistics/logistics.db \
  "SELECT 'product_catalog (preserved)', count(*) FROM product_catalog UNION ALL
   SELECT 'product', count(*) FROM product UNION ALL
   SELECT 'work_history', count(*) FROM work_history UNION ALL
   SELECT 'image_file', count(*) FROM image_file UNION ALL
   SELECT 'device_status', count(*) FROM device_status UNION ALL
   SELECT 'error_log', count(*) FROM error_log UNION ALL
   SELECT 'mqtt_event_log', count(*) FROM mqtt_event_log UNION ALL
   SELECT 'security_log', count(*) FROM security_log UNION ALL
   SELECT 'http_upload', count(*) FROM http_upload UNION ALL
   SELECT 'process_work_state', count(*) FROM process_work_state UNION ALL
   SELECT 'process_gripper_target', count(*) FROM process_gripper_target UNION ALL
   SELECT 'command_outbox', count(*) FROM process_command_outbox UNION ALL
   SELECT 'mqtt_outbox', count(*) FROM process_mqtt_outbox UNION ALL
   SELECT 'processed_message', count(*) FROM process_processed_message UNION ALL
   SELECT 'pending_command', count(*) FROM command_manager_pending UNION ALL
   SELECT 'completed_command', count(*) FROM command_manager_completed UNION ALL
   SELECT 'command_manager_runtime (new session row allowed)', count(*) FROM command_manager_runtime UNION ALL
   SELECT 'pending_system_command', count(*) FROM pending_system_command UNION ALL
   SELECT 'process_runtime_state (new session row allowed)', count(*) FROM process_runtime_state UNION ALL
   SELECT 'schema_migrations (current metadata allowed)', count(*) FROM schema_migrations;" > "${FRESH_RUNTIME_EVIDENCE}"

# Assert: product_catalog equals the pre-fresh count; every unlabelled runtime/event/history row is 0.
# process_runtime_state and command_manager_runtime are new-session rows (0 or 1); schema_migrations is current metadata.
awk -F '|' -v catalog="${CATALOG_COUNT_BEFORE}" '
  $1 == "product_catalog (preserved)" { if ($2 != catalog) bad = 1; seen_catalog = 1; next }
  $1 == "process_runtime_state (new session row allowed)" ||
  $1 == "command_manager_runtime (new session row allowed)" { if ($2 !~ /^[01]$/) bad = 1; next }
  $1 == "schema_migrations (current metadata allowed)" { if ($2 !~ /^[0-9]+$/) bad = 1; next }
  $2 != "0" { bad = 1 }
  END { exit !(seen_catalog && !bad) }
' "${FRESH_RUNTIME_EVIDENCE}"

# Each Pi terminal first sources/validates its local absolute RUN_ENV using the block above, then checks its own
# exact role/device/inbound spool path and ownership.
SPOOL_EVIDENCE="${EVIDENCE_DIR}/spool.txt"
set +e
set -o pipefail
sudo find "${RUN_SPOOL}" -printf '%M %u:%g %p\n' | sort | tee "${SPOOL_EVIDENCE}"
spool_statuses=("${PIPESTATUS[@]}")
set -e
test "${spool_statuses[0]}" -eq 0
test "${spool_statuses[1]}" -eq 0
test "${spool_statuses[2]}" -eq 0

# Broker listener와 서비스 상태
sudo systemctl status mosquitto --no-pager
sudo ss -ltnp 'sport = :8883'

# At test end, source/validate the absolute RUN_ENV again, then persist the UTC end bound for journalctl.
RUN_END="$(date --utc '+%Y-%m-%d %H:%M:%S')"
umask 077
{
  printf "RUN_ID='%s'\n" "${RUN_ID}"
  printf "SPOOL_BASE_REAL='%s'\nRUN_SPOOL='%s'\n" "${SPOOL_BASE_REAL}" "${RUN_SPOOL}"
  printf "EVIDENCE_DIR='%s'\nRUN_START='%s'\nRUN_END='%s'\n" "${EVIDENCE_DIR}" "${RUN_START}" "${RUN_END}"
} > "${RUN_ENV}"
chmod 0600 "${RUN_ENV}"
test -n "${RUN_START}"
test -n "${RUN_END}"

# UTC journal transport-receipt evidence. Use short-iso-precise only when supported; its precision is microseconds,
# not nanoseconds. Redirected capture preserves output and propagates journalctl failure under `set -e`.
if journalctl --help | grep -q short-iso-precise; then JOURNAL_FORMAT=short-iso-precise; else JOURNAL_FORMAT=short-iso; fi
sudo journalctl --utc -u mosquitto --since "${RUN_START}" --until "${RUN_END}" \
  -o "${JOURNAL_FORMAT}" --no-pager > "${EVIDENCE_DIR}/broker.journal.log"
sudo journalctl --utc -u logistics-central-server -u logistics-input-node -u logistics-sorting-node \
  --since "${RUN_START}" --until "${RUN_END}" -o "${JOURNAL_FORMAT}" --no-pager > "${EVIDENCE_DIR}/services.journal.log"
```

Vision과 gripper에는 systemd unit이 없다. 서로 다른 Pi/terminal에서 각각 독립적으로 foreground stdout/stderr를
캡처한다. stdout capture에는 timestamp를 덧붙이지 않는다. timeline의 ns 근거는 application/MQTT payload 또는
실행 전후와 수동 ESTOP/RECOVERY에 찍는 UTC ns operator marker뿐이다. 아래에서 operator SIGINT에 따른 `130`은
해당 run의 다른 pass 기준을 이미 충족했다고 operator가 명시적으로 확인한 경우에만 정상 capture close로 처리한다.

```bash
# Vision Pi terminal only. First source and validate this Pi's recorded absolute RUN_ENV using the block above.
set -e
marker() { log_file="$1"; shift; printf '%s %s\n' "$(date -u --iso-8601=ns)" "$*" | tee -a "${log_file}"; }
marker "${EVIDENCE_DIR}/vision.log" 'vision foreground start'
# Leave CAPTURE_CLOSE_AFTER_PASS unset during the run. Set it to yes in this terminal only after all other pass
# criteria are recorded, immediately before the expected Ctrl-C close.
set +e
set -o pipefail
stdbuf -oL -eL ./build-vision/device-rpi/logistics_vision_node --headless \
  --config runtime/vision-node/vision-node.ini 2>&1 | tee -a "${EVIDENCE_DIR}/vision.log"
vision_statuses=("${PIPESTATUS[@]}")
set -e
test "${vision_statuses[1]}" -eq 0
case "${vision_statuses[0]}" in
  0) ;;
  130) test "${CAPTURE_CLOSE_AFTER_PASS:-}" = yes; marker "${EVIDENCE_DIR}/vision.log" 'operator SIGINT capture close after pass confirmation' ;;
  *) marker "${EVIDENCE_DIR}/vision.log" "vision exited ${vision_statuses[0]}"; exit "${vision_statuses[0]}" ;;
esac
```

```bash
# Gripper Pi terminal only. First source and validate this Pi's recorded absolute RUN_ENV using the block above.
set -e
marker() { log_file="$1"; shift; printf '%s %s\n' "$(date -u --iso-8601=ns)" "$*" | tee -a "${log_file}"; }
marker "${EVIDENCE_DIR}/gripper.log" 'gripper foreground start'
: "${GRIPPER_BINARY:?set GRIPPER_BINARY to the deployed logistics_gripper_node executable}"
GRIPPER_CONFIG="${GRIPPER_CONFIG:-runtime/gripper-node/gripper-node.ini}"
test -x "${GRIPPER_BINARY}"
test -r "${GRIPPER_CONFIG}"
# Leave CAPTURE_CLOSE_AFTER_PASS unset during the run. Set it to yes in this terminal only after all other pass
# criteria are recorded, immediately before the expected Ctrl-C close.
set +e
stdbuf -oL -eL "${GRIPPER_BINARY}" "${GRIPPER_CONFIG}" /dev/vedauart 2>&1 | tee -a "${EVIDENCE_DIR}/gripper.log"
gripper_statuses=("${PIPESTATUS[@]}")
set -e
test "${gripper_statuses[1]}" -eq 0
case "${gripper_statuses[0]}" in
  0) ;;
  130) test "${CAPTURE_CLOSE_AFTER_PASS:-}" = yes; marker "${EVIDENCE_DIR}/gripper.log" 'operator SIGINT capture close after pass confirmation' ;;
  *) marker "${EVIDENCE_DIR}/gripper.log" "gripper exited ${gripper_statuses[0]}"; exit "${gripper_statuses[0]}" ;;
esac
```

```sh
# From a second operator terminal, source and validate the recorded absolute RUN_ENV using the block above; it has no
# shell-local marker() function from the capture terminal. Use absolute evidence paths for START, every ESTOP/RECOVERY,
# and process-exit markers.
printf '%s %s\n' "$(date -u --iso-8601=ns)" 'ESTOP sent at vision stage' | tee -a "${EVIDENCE_DIR}/vision.log"
printf '%s %s\n' "$(date -u --iso-8601=ns)" 'RECOVERY acknowledged at gripper stage' | tee -a "${EVIDENCE_DIR}/gripper.log"
```

Broker 메시지 관찰은 TLS와 ACL이 적용된 승인된 관찰자 자격 증명으로 별도 보안 절차에 따라 실행하고, topic, payload,
payload timestamp precision, process epoch를 증거에 저장한다. broker journal은 연결/재연결과 ACL 거부의 UTC
microsecond transport receipt로 보관한다.

## 통과/실패 기준과 롤백

통과하려면 세 상자 모두에서 input 정지 → vision 성공 → gripper HOME terminal response → sorting 목적지/시작 →
감지/정지/완료 순서가 지켜져야 한다. 그리퍼 HOME 전 input 재시작, sorting 시작 전 HOME 응답 누락, 중복 완료,
이전 epoch 메시지 또는 spool 재생, RECOVERY 뒤 active work/pending command 존재는 실패다.

실패하면 즉시 ESTOP을 유지하고 신규 상자 투입과 `START`를 중단한다. `RUN_ID`, process epoch, SQLite 조회,
spool 목록, broker/서비스 journal을 보존한다. 승인된 이전 이미지와 중앙 artifact로 되돌린 뒤 `startup_mode=fresh`로
재시작하고, 새 spool run 디렉터리와 새 epoch에서만 재시험한다. STM32는 UART 계약 불일치가 실측으로 확인될 때까지
재플래시하지 않는다.
