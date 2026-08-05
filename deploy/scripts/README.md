# Raspberry Pi 서비스 설치 스크립트

중앙 서버와 장치 노드 setup 스크립트는 `logistics` 시스템 계정을 준비하고, 필요한 타깃을 빌드한 뒤 아래 고정 배포
경로에 실행 파일·설정·systemd unit을 설치합니다. unit을 활성화하고 서비스를 재시작하며, 기존 INI는
`LOGISTICS_FORCE_CONFIG=1`을 지정하지 않는 한 보존합니다.

| 항목 | 경로 |
| --- | --- |
| 실행 파일과 migration | `/opt/logistics-automation` |
| 서비스 설정 | `/etc/logistics` |
| 영속 데이터 | `/var/lib/logistics` |
| 활성 systemd unit | `/etc/systemd/system` |

이 경로들은 systemd unit의 `ExecStart`, `WorkingDirectory`, `StateDirectory`와 일치해야 하므로 setup 환경변수로 변경할 수
없습니다. 빌드 디렉터리만 `LOGISTICS_BUILD_DIR`로 변경할 수 있습니다.

## 중앙서버

```bash
(
cleanup_mqtt_password() { unset LOGISTICS_MQTT_PASSWORD; }
trap cleanup_mqtt_password EXIT
trap 'exit 130' HUP INT TERM
read -rsp 'central-server MQTT password: ' LOGISTICS_MQTT_PASSWORD; printf '\n'
export LOGISTICS_MQTT_PASSWORD
export LOGISTICS_UPLOAD_TOKEN='충분히-긴-임의의-토큰'
export LOGISTICS_MQTT_HOST='mqtt.logistics.local'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-central-server.sh
)
```

주요 설치 파일:

```text
/opt/logistics-automation/bin/logistics_central_server
/opt/logistics-automation/share/logistics/migrations/
/etc/logistics/server.ini
/etc/systemd/system/logistics-central-server.service
/var/lib/logistics/logistics.db
/var/lib/logistics/uploads/
```

이 스크립트는 Mosquitto 설정을 수정하지 않습니다. `mosquitto.service`가 준비된 상태에서
`logistics-central-server.service`를 활성화하고 재시작합니다.

## RTSP 릴레이

승인된 네 채널은 같은 CCTV endpoint를 사용하고 경로만 다릅니다. 계정 정보는 표에 포함하지 않습니다.

| 릴레이 경로 | CCTV host:port | CCTV 경로 | Qt에서 사용할 relay URL |
| --- | --- | --- | --- |
| `channel1` | `veda4-t4.iptime.org:8554` | `/0/profile2/media.smp` | `rtsp://172.20.33.72:8554/channel1` |
| `channel2` | `veda4-t4.iptime.org:8554` | `/1/profile2/media.smp` | `rtsp://172.20.33.72:8554/channel2` |
| `channel3` | `veda4-t4.iptime.org:8554` | `/2/profile2/media.smp` | `rtsp://172.20.33.72:8554/channel3` |
| `channel4` | `veda4-t4.iptime.org:8554` | `/3/profile2/media.smp` | `rtsp://172.20.33.72:8554/channel4` |

ARM64 중앙 Raspberry Pi에서 처음 설치할 때 서비스 계정을 멱등하게 준비합니다.

```sh
sudo -v
getent group logistics >/dev/null || sudo groupadd --system logistics
id logistics >/dev/null 2>&1 || sudo useradd --system --gid logistics \
  --home-dir /nonexistent --shell /usr/sbin/nologin logistics
getent group logistics
id logistics
```

그 다음 보안 채널로 전달받은 네 CCTV URL과 relay 계정을 같은 shell의 non-echoing prompt로 입력합니다. 네 URL은 위
host, port와 경로를 사용하되 같은 CCTV 계정을 포함한 완전한 값이어야 합니다. relay username은 `control-centor`를
입력합니다. `read -s`를 사용하는 아래 block은 Bash에서 실행해야 합니다. subshell 안에서만 변수와 trap을 만들므로 기존
interactive shell 상태는 바뀌지 않습니다. 값은 shell history에 쓰지 않으며 setup 성공 여부와 관계없이 즉시 제거합니다.

```bash
(
  cleanup_rtsp_env() {
    unset LOGISTICS_RTSP_SOURCE_1 LOGISTICS_RTSP_SOURCE_2 \
      LOGISTICS_RTSP_SOURCE_3 LOGISTICS_RTSP_SOURCE_4 \
      LOGISTICS_RTSP_RELAY_USER LOGISTICS_RTSP_RELAY_PASSWORD
  }
  trap cleanup_rtsp_env EXIT
  trap 'cleanup_rtsp_env; exit 129' HUP
  trap 'cleanup_rtsp_env; exit 130' INT
  trap 'cleanup_rtsp_env; exit 143' TERM

  printf 'CCTV channel 1 URL: '; read -rs LOGISTICS_RTSP_SOURCE_1; printf '\n'
  printf 'CCTV channel 2 URL: '; read -rs LOGISTICS_RTSP_SOURCE_2; printf '\n'
  printf 'CCTV channel 3 URL: '; read -rs LOGISTICS_RTSP_SOURCE_3; printf '\n'
  printf 'CCTV channel 4 URL: '; read -rs LOGISTICS_RTSP_SOURCE_4; printf '\n'
  printf 'Relay username: '; read -rs LOGISTICS_RTSP_RELAY_USER; printf '\n'
  printf 'Relay password: '; read -rs LOGISTICS_RTSP_RELAY_PASSWORD; printf '\n'
  export LOGISTICS_RTSP_SOURCE_1 LOGISTICS_RTSP_SOURCE_2 \
    LOGISTICS_RTSP_SOURCE_3 LOGISTICS_RTSP_SOURCE_4 \
    LOGISTICS_RTSP_RELAY_USER LOGISTICS_RTSP_RELAY_PASSWORD

  if ./deploy/scripts/setup-rtsp-relay.sh; then
    setup_status=0
  else
    setup_status=$?
  fi
  exit "${setup_status}"
)
```

스크립트는 ARM64용 MediaMTX를 `1.19.3`으로 고정해 내려받고, 배포된 `checksums.sha256`으로 아카이브를 검증한 뒤에만
바이너리를 설치합니다. `/etc/logistics/rtsp-relay.yml`이 이미 있으면 기본적으로 보존합니다. 카메라 소스를 바꾸려면
위와 같이 여섯 값을 다시 입력하고 setup 실행 전에 `export LOGISTICS_FORCE_CONFIG=1`을 추가합니다. 실행 후에는 이 변수도
제거하고 릴레이를 재시작합니다.

```sh
export LOGISTICS_FORCE_CONFIG=1
# 위 non-echoing 입력과 setup block을 다시 실행합니다.
unset LOGISTICS_FORCE_CONFIG
sudo systemctl restart logistics-rtsp-relay
```

실제 카메라 URL과 릴레이 비밀번호는 권한이 제한된 `/etc/logistics/rtsp-relay.yml`과 커밋하지 않는 Qt 런타임 INI에만
둡니다.

## Vision Raspberry Pi

```bash
(
cleanup_mqtt_password() { unset LOGISTICS_MQTT_PASSWORD; }
trap cleanup_mqtt_password EXIT
trap 'exit 130' HUP INT TERM
read -rsp 'PI-VISION-01 MQTT password: ' LOGISTICS_MQTT_PASSWORD; printf '\n'
export LOGISTICS_MQTT_PASSWORD
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_MQTT_HOST='mqtt.logistics.local'
export LOGISTICS_UPLOAD_TOKEN='중앙서버와-동일한-토큰'
export LOGISTICS_DEVICE_ID='PI-VISION-01'
export LOGISTICS_DEVICE_IP='192.168.0.21'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-vision-node.sh
)
```

OpenCV 4.10.0이 없으면 소스 빌드를 명시적으로 허용합니다.

```sh
export LOGISTICS_INSTALL_OPENCV=1
./deploy/scripts/setup-vision-node.sh
```

실행 파일은 `/opt/logistics-automation/bin/logistics_vision_node`, 설정은
`/etc/logistics/vision-node.ini`에 설치됩니다. 스크립트는 `logistics-vision-node.service`를 활성화하고 재시작합니다.

The setup scripts assume build dependencies are already installed. To install the required Ubuntu packages as part of
the run, explicitly opt in:

```sh
export LOGISTICS_INSTALL_DEPENDENCIES=1
```

## 3. Input conveyor Raspberry Pi

The input node bridges MQTT commands to the input-controller STM32 over the `/dev/vedauart` character device and
reports sensor/motor status back to the central server.

```bash
(
cleanup_mqtt_password() { unset LOGISTICS_MQTT_PASSWORD; }
trap cleanup_mqtt_password EXIT
trap 'exit 130' HUP INT TERM
read -rsp 'PI-INPUT-01 MQTT password: ' LOGISTICS_MQTT_PASSWORD; printf '\n'
export LOGISTICS_MQTT_PASSWORD
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_MQTT_HOST='mqtt.logistics.local'
export LOGISTICS_DEVICE_ID='PI-INPUT-01'
export LOGISTICS_DEVICE_IP='192.168.0.22'
export LOGISTICS_UART_DEVICE='/dev/vedauart'
./deploy/scripts/setup-input-node.sh
)
```

The binary is installed at `/opt/logistics-automation/bin/logistics_input_node` and the configuration at
`/etc/logistics/input-node.ini`. The script enables and restarts `logistics-input-node.service`. The unit uses
`/dev/vedauart`, so the setup script rejects another `LOGISTICS_UART_DEVICE` value. The script configures with
`LOGISTICS_BUILD_VISION_NODE=OFF`, `LOGISTICS_BUILD_SORTING_NODE=OFF`, and
`LOGISTICS_BUILD_LINETRACER_NODE=OFF`, so OpenCV is not required to build the input node.

## Sorting 및 Line Tracer Raspberry Pi

두 UART 노드는 Input Node와 같은 MQTT 환경변수와 `/dev/vedauart`를 사용합니다. MQTT 비밀번호는 위 예제처럼 subshell의
non-echoing prompt에서 입력한 뒤 해당 setup 스크립트를 실행합니다.

| 노드 | setup 스크립트 | 기본 장치 ID | 설정 | 서비스 |
| --- | --- | --- | --- | --- |
| Sorting | `setup-sorting-node.sh` | `PI-SORTING-01` | `/etc/logistics/sorting-node.ini` | `logistics-sorting-node.service` |
| Line Tracer | `setup-linetracer-node.sh` | `PI-LT-01` | `/etc/logistics/linetracer-node.ini` | `logistics-linetracer-node.service` |

```bash
(
cleanup_mqtt_password() { unset LOGISTICS_MQTT_PASSWORD; }
trap cleanup_mqtt_password EXIT
trap 'exit 130' HUP INT TERM
read -rsp 'device MQTT password: ' LOGISTICS_MQTT_PASSWORD; printf '\n'
export LOGISTICS_MQTT_PASSWORD
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_MQTT_HOST='mqtt.logistics.local'
export LOGISTICS_DEVICE_IP='장치-IP'
export LOGISTICS_DEVICE_ID='PI-SORTING-01'
./deploy/scripts/setup-sorting-node.sh
)
```

Line Tracer를 설치할 때는 마지막 두 줄의 장치 ID와 스크립트를 각각 `PI-LT-01`,
`setup-linetracer-node.sh`로 바꿉니다.

두 스크립트는 해당 실행 파일과 unit을 설치하고 서비스를 활성화·재시작합니다. 하나의 Raspberry Pi에서 UART를 공유하는
서비스를 검증할 때는 동시에 활성화하지 않습니다.

## 연결 검사

Run this from the Vision Pi after the MQTT broker and central server have started:

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
./deploy/scripts/check-connectivity.sh
```

`check-connectivity.sh`의 TCP 검사는 서비스 포트 접근성만 확인합니다. MQTT TLS·인증·ACL은
[Mosquitto TLS 가이드](../mosquitto/README.md)의 `mosquitto_sub`, `mosquitto_pub`, `openssl s_client`로 검증합니다.

## 선택 환경 변수

| 변수 | 용도 |
| --- | --- |
| `LOGISTICS_BUILD_DIR` | CMake 빌드 경로 |
| `LOGISTICS_NODE_NAME` | 장치 등록 이름 |
| `LOGISTICS_MQTT_HOST` | 서버 인증서 SAN과 일치하는 broker DNS 이름 또는 IP |
| `LOGISTICS_MQTT_PORT` | MQTT listener, 기본값 `8883` |
| `LOGISTICS_MQTT_USERNAME` | password file 사용자, 기본값은 component/client ID |
| `LOGISTICS_MQTT_PASSWORD` | MQTT 비밀번호, 새 INI 생성 시 필수 |
| `LOGISTICS_MQTT_TLS_ENABLED` | `true` 또는 `false`, 기본값 `true` |
| `LOGISTICS_MQTT_CA_CERTIFICATE` | 공개 CA 경로, 기본값 `/etc/logistics/tls/ca.crt` |
| `LOGISTICS_UPLOAD_TOKEN` | 중앙 서버 업로드 bearer token, 중앙 서버와 Vision의 새 INI 생성 시 필수 |
| `LOGISTICS_UART_DEVICE` | UART 장치, systemd 배포에서는 `/dev/vedauart`만 지원 |
| `LOGISTICS_SORTING_DEFAULT_SPEED` | Sorting 기본 속도, `1`부터 `100`까지 |
| `LOGISTICS_FORCE_CONFIG=1` | 기존 INI 덮어쓰기 |
| `LOGISTICS_INSTALL_DEPENDENCIES=1` | apt 의존성 설치 |
| `LOGISTICS_INSTALL_OPENCV=1` | OpenCV 4.10.0 소스 설치 |

토큰과 비밀번호를 스크립트 파일에 직접 기록하지 않습니다. 여러 값을 연속 입력하는 자동화에서는
`feature/rtsp-relay`의 설치 절차처럼 subshell 안에서 `read -s`로 입력하고 `trap`으로 환경변수를 해제합니다.
TLS를 끄고 임시 `1883` listener를 사용하더라도 `allow_anonymous false`가 적용되므로 사용자명과 비밀번호는 필요합니다.
