# 공정 워크플로 하드웨어 검증

## 상태

**NOT YET physically executed.** 이 문서는 실제 장비 시험의 실행 기록과 배포 판정을 위한 절차다. 아래 표의
결과와 증거는 시험을 수행한 뒤에만 채운다.

## 사전 조건

- 중앙 서버는 `startup_mode=fresh`로 시작한다. fresh 시작 뒤에는 `product_catalog`만 보존하고 이전 작업, 명령,
  inbox/outbox, 처리 메시지는 남아 있지 않아야 한다. 새 세션의 `STOPPED` 공정 상태와 새 epoch는 생성될 수 있다.
- 중앙 서버, input, vision, gripper, sorting 장비 ID와 TLS/ACL 연결을 확인한다. 이 문서와 셸 이력에는
  비밀번호, 토큰, 개인키를 기록하지 않는다.
- 시험마다 새 `RUN_ID`와 중앙이 만든 새 process epoch를 기록한다. input/sorting/vision/gripper의 MQTT
  outbound 및 inbound spool은 기존 내용을 재사용하지 않는 별도 run 디렉터리로 설정한다. 예:

  ```sh
  sudo install -d -o logistics -g logistics -m 0750 \
    /var/lib/logistics/mqtt-spool/${RUN_ID}/{input,sorting,vision,gripper}
  ```

  설정한 각 노드의 `publish_spool_directory`가 해당 run 디렉터리를 사용하고, 이전 spool 경로가 비어 있거나
  격리된 것을 시작 전에 확인한다. spool을 삭제하는 절차는 운영자 승인과 백업 정책을 따른다.

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
2. 실제 상자 세 개를 한 번에 하나씩 통과시키고, 각 이벤트의 발생 또는 중앙 수신 시각을 ISO-8601 UTC
   나노초(`YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ`)로 기록한다.
3. 아래 필드가 빠지거나 순서가 어긋나면 즉시 실패로 표시하고 해당 `RUN_ID`에서 추가 상자를 투입하지 않는다.

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

```sh
# 중앙 SQLite: fresh 세션에 catalog 외 공정 런타임 데이터가 남지 않았는지 확인
sudo sqlite3 /var/lib/logistics/logistics.db \
  "SELECT 'catalog', count(*) FROM product_catalog UNION ALL
   SELECT 'work', count(*) FROM process_work_state UNION ALL
   SELECT 'command_outbox', count(*) FROM process_command_outbox UNION ALL
   SELECT 'mqtt_outbox', count(*) FROM process_mqtt_outbox UNION ALL
   SELECT 'pending_command', count(*) FROM command_manager_pending;"

# 각 Pi에서 RUN_ID spool만 사용하며 pending 파일이 남는지 확인
sudo find /var/lib/logistics/mqtt-spool/${RUN_ID} -type f -printf '%p %TY-%Tm-%TdT%TTZ\n' | sort

# Broker listener와 서비스 상태
sudo systemctl status mosquitto --no-pager
sudo ss -ltnp 'sport = :8883'

# Broker, 서버, 노드 journal의 UTC 나노초 로그를 보존
sudo journalctl -u mosquitto --since "${RUN_START}" --until "${RUN_END}" -o short-precise --no-pager
sudo journalctl -u logistics-central-server --since "${RUN_START}" --until "${RUN_END}" -o short-precise --no-pager
sudo journalctl -u logistics-input-node -u logistics-sorting-node -u logistics-vision-node \
  -u logistics-gripper-node --since "${RUN_START}" --until "${RUN_END}" -o short-precise --no-pager
```

Broker 메시지 관찰은 TLS와 ACL이 적용된 승인된 관찰자 자격 증명으로 별도 보안 절차에 따라 실행하고, topic, payload,
수신 UTC 나노초와 process epoch를 증거에 저장한다. broker 로그에는 해당 `RUN_ID`의 연결/재연결과 ACL 거부도
함께 보관한다.

## 통과/실패 기준과 롤백

통과하려면 세 상자 모두에서 input 정지 → vision 성공 → gripper HOME terminal response → sorting 목적지/시작 →
감지/정지/완료 순서가 지켜져야 한다. 그리퍼 HOME 전 input 재시작, sorting 시작 전 HOME 응답 누락, 중복 완료,
이전 epoch 메시지 또는 spool 재생, RECOVERY 뒤 active work/pending command 존재는 실패다.

실패하면 즉시 ESTOP을 유지하고 신규 상자 투입과 `START`를 중단한다. `RUN_ID`, process epoch, SQLite 조회,
spool 목록, broker/서비스 journal을 보존한다. 승인된 이전 이미지와 중앙 artifact로 되돌린 뒤 `startup_mode=fresh`로
재시작하고, 새 spool run 디렉터리와 새 epoch에서만 재시험한다. STM32는 UART 계약 불일치가 실측으로 확인될 때까지
재플래시하지 않는다.
