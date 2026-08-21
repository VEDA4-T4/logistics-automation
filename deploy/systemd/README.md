# 중앙 서버 systemd 운영

이 디렉터리의 직접 설치 대상은 중앙 서버입니다. Input, Vision, Gripper, Sorting, Line Tracer 노드는 노드별 Yocto 이미지에
애플리케이션과 systemd unit을 함께 패키징합니다.

## 운영 경로

| 항목 | 경로 |
| --- | --- |
| 실행 파일 | `/opt/logistics-automation/bin/logistics_central_server` |
| migration | `/opt/logistics-automation/share/logistics/migrations` |
| 설정 | `/etc/logistics/server.ini` |
| 영속 데이터 | `/var/lib/logistics` |
| 활성 unit | `/etc/systemd/system/logistics-central-server.service` |

`deploy/scripts/setup-central-server.sh`는 `logistics` 시스템 계정과 운영 디렉터리를 준비하고, 중앙 서버를 빌드·테스트한 뒤
위 경로에 설치합니다. 기존 설정은 `LOGISTICS_FORCE_CONFIG=1`을 지정하지 않는 한 보존합니다.

## 서비스 의존성과 복구

중앙 서버는 네트워크와 Mosquitto가 준비된 뒤 시작됩니다. Mosquitto가 시작되지 않으면 중앙 서버도 시작되지 않습니다.
프로세스가 비정상 종료되면 systemd가 3초 후 재시작하며, 관리자가 `systemctl stop`으로 정상 중지한 경우에는 자동으로 다시
시작하지 않습니다.

중앙 서버는 `SIGTERM`을 받으면 MQTT와 HTTP 수신을 중지하고 공정 상태 저장 및 SQLite WAL 체크포인트를 수행합니다. 재시작
후 진행 중이던 공정은 장비를 자동 구동하지 않고 `STOPPED` 상태로 복원되므로 현장 상태를 확인한 뒤 시작해야 합니다.

## 설치

MQTT 비밀번호와 HTTP 업로드 토큰을 현재 shell에서만 주입한 뒤 setup 스크립트를 실행합니다.

```bash
(
cleanup_secrets() { unset LOGISTICS_MQTT_PASSWORD LOGISTICS_UPLOAD_TOKEN; }
trap cleanup_secrets EXIT
trap 'exit 130' HUP INT TERM
read -rsp 'central-server MQTT password: ' LOGISTICS_MQTT_PASSWORD; printf '\n'
read -rsp 'central-server upload token: ' LOGISTICS_UPLOAD_TOKEN; printf '\n'
export LOGISTICS_MQTT_PASSWORD LOGISTICS_UPLOAD_TOKEN
export LOGISTICS_MQTT_HOST='mqtt.logistics.local'
./deploy/scripts/setup-central-server.sh
)
```

스크립트는 Mosquitto 설정을 수정하거나 Mosquitto 서비스를 재시작하지 않습니다. 비밀번호, 토큰과 개인키를 unit의
`Environment=`에 직접 넣지 않고 권한이 제한된 `/etc/logistics/server.ini` 또는 별도 credentials 파일로 관리합니다.

## 상태와 로그

```sh
systemctl is-enabled logistics-central-server
systemctl is-active logistics-central-server
systemctl status logistics-central-server --no-pager
journalctl -u logistics-central-server -f
```

설정 변경 후에는 중앙 서버만 재시작합니다.

```sh
sudo systemctl restart logistics-central-server
```

코드 또는 unit을 갱신할 때 setup 스크립트를 다시 실행하면 새 빌드를 테스트·설치하고 서비스를 재시작합니다. 기존
`/etc/logistics/server.ini`와 `/var/lib/logistics` 데이터는 보존됩니다.
