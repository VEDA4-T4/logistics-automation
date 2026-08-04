# 중앙서버와 Vision 설치 스크립트

스크립트는 저장소 루트의 `runtime/` 아래에 설정과 생성 데이터를 만들고 필요한 타깃을 빌드합니다. 기존 INI는
`LOGISTICS_FORCE_CONFIG=1`을 지정하지 않는 한 보존합니다.

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

생성 파일:

```text
runtime/central-server/server.ini
runtime/central-server/logistics.db
runtime/central-server/uploads/
```

이 스크립트는 Mosquitto 설정을 수정하거나 서비스를 재시작하지 않습니다.

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

생성 설정은 `runtime/vision-node/vision-node.ini`입니다.

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

The generated runtime configuration is `runtime/input-node/input-node.ini`. The daemon is started manually (or by a
future systemd unit) with the UART device supplied through `LOGISTICS_UART_DEVICE` or as the second argument. The
script configures with `LOGISTICS_BUILD_VISION_NODE=OFF`, `LOGISTICS_BUILD_SORTING_NODE=OFF`, and
`LOGISTICS_BUILD_LINETRACER_NODE=OFF`, so OpenCV is not required to build the input node.

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
| `LOGISTICS_CONFIG_PATH` | 생성·사용할 INI 경로 |
| `LOGISTICS_BUILD_DIR` | CMake 빌드 경로 |
| `LOGISTICS_RUNTIME_DIR` | 런타임 데이터 경로 |
| `LOGISTICS_NODE_NAME` | 장치 등록 이름 |
| `LOGISTICS_MQTT_HOST` | 서버 인증서 SAN과 일치하는 broker DNS 이름 또는 IP |
| `LOGISTICS_MQTT_PORT` | MQTT listener, 기본값 `8883` |
| `LOGISTICS_MQTT_USERNAME` | password file 사용자, 기본값은 component/client ID |
| `LOGISTICS_MQTT_PASSWORD` | MQTT 비밀번호, 새 INI 생성 시 필수 |
| `LOGISTICS_MQTT_TLS_ENABLED` | `true` 또는 `false`, 기본값 `true` |
| `LOGISTICS_MQTT_CA_CERTIFICATE` | 공개 CA 경로, 기본값 `/etc/logistics/tls/ca.crt` |
| `LOGISTICS_FORCE_CONFIG=1` | 기존 INI 덮어쓰기 |
| `LOGISTICS_INSTALL_DEPENDENCIES=1` | apt 의존성 설치 |
| `LOGISTICS_INSTALL_OPENCV=1` | OpenCV 4.10.0 소스 설치 |

토큰과 비밀번호를 스크립트 파일에 직접 기록하지 않습니다. 여러 값을 연속 입력하는 자동화에서는
`feature/rtsp-relay`의 설치 절차처럼 subshell 안에서 `read -s`로 입력하고 `trap`으로 환경변수를 해제합니다.
TLS를 끄고 임시 `1883` listener를 사용하더라도 `allow_anonymous false`가 적용되므로 사용자명과 비밀번호는 필요합니다.
