# Central Server

중앙 Raspberry Pi에서 실행하며 MQTT 메시지 검증·저장·라우팅, 장치/명령 관리, 공정 상태 머신, SQLite 이력과
HTTP(S) 이미지·로그 업로드를 담당합니다. Mosquitto broker 자체는 애플리케이션에 내장하지 않습니다.

## 설치와 빌드

```sh
export LOGISTICS_UPLOAD_TOKEN='충분히-긴-임의의-토큰'
export LOGISTICS_MQTT_HOST='127.0.0.1'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-central-server.sh
```

수동 빌드는 [빌드 및 테스트](../docs/guides/compilation-and-tests.md)를 참고합니다.

## 설정

기준 파일은 `config/server.ini.example`, 로컬 권장 경로는 `runtime/central-server/server.ini`입니다.

주요 section:

- `[mqtt]`: 외부 Mosquitto 접속
- `[device_registry]`: 등록 장치 snapshot
- `[database]`: SQLite와 migration
- `[storage]`: 이력·이미지 보존 기간
- `[http]`: 이미지/로그 업로드와 조회
- `[routing]`: Control Center client ID
- `[process]`: 공정 오케스트레이터와 장치 ID

상세 설정은 [런타임 설정](../docs/guides/runtime-configuration.md)을 참고하세요.

## 실행

```sh
./build-central/central-server-rpi/logistics_central_server \
  --config runtime/central-server/server.ini
```

설정 경로는 위치 인자, `--config`, `--config=...`, `LOGISTICS_CENTRAL_SERVER_CONFIG`를 지원합니다.

`registered devices=0`으로 시작하는 것은 정상입니다. 각 Device 노드가 MQTT registration을 발행하면 증가합니다.

## 공정 오케스트레이터

모델:

```text
Input → Vision → Gripper → Sorting → Line Tracer → Completed
```

모든 장치별 MQTT↔UART 구현이 준비되기 전에는 `[process] enabled=false`를 유지합니다. 준비되지 않은 장치로 명령을
보내지 못하면 작업과 시스템 상태가 `ERROR`로 전환됩니다.

## 관련 문서

- [MQTT 계약](../shared/contracts/mqtt/README.md)
- [HTTP 업로드 서버](src/http_upload/README.md)
- [DB migration](db/migrations/README.md)
- [Mosquitto 보안 및 TLS](../deploy/mosquitto/README.md)
- [systemd 운영](../deploy/systemd/README.md)

## 설정 검증과 재시작 복구

서버는 시작할 때 알 수 없는 섹션·키, 중복 키, 범위를 벗어난 숫자, 잘못된 장치 ID와
HTTP 인증/TLS 조합을 거부합니다. 상대 경로는 `server.ini`가 있는 디렉터리를 기준으로
해석됩니다.

고정된 `client_id`와 `clean_session=false`를 사용하면 서버가 중단된 동안 브로커에 보관된
QoS 1 메시지를 다시 받을 수 있습니다. Mosquitto 브로커도 persistence가 활성화되어 있어야
합니다.

공정 진행 상태와 내부 명령 시퀀스는 SQLite에 저장됩니다. 서버 재시작 후 진행 중이던 공정은
장비를 임의로 다시 움직이지 않도록 `STOPPED`로 복원됩니다. 비상 정지와 오류 상태는 유지되며,
운영자가 현장 상태를 확인한 뒤 복구 또는 시작 명령을 내려야 합니다.
