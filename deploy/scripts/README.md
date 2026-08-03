# 중앙서버와 Vision 설치 스크립트

스크립트는 저장소 루트의 `runtime/` 아래에 설정과 생성 데이터를 만들고 필요한 타깃을 빌드합니다. 기존 INI는
`LOGISTICS_FORCE_CONFIG=1`을 지정하지 않는 한 보존합니다.

## 중앙서버

```sh
export LOGISTICS_UPLOAD_TOKEN='충분히-긴-임의의-토큰'
export LOGISTICS_MQTT_HOST='127.0.0.1'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-central-server.sh
```

생성 파일:

```text
runtime/central-server/server.ini
runtime/central-server/logistics.db
runtime/central-server/uploads/
```

이 스크립트는 Mosquitto 설정을 수정하거나 서비스를 재시작하지 않습니다.

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
입력합니다. 값은 shell history에 쓰지 않으며 setup 성공 여부와 관계없이 즉시 제거합니다.

```sh
cleanup_rtsp_env() {
  unset LOGISTICS_RTSP_SOURCE_1 LOGISTICS_RTSP_SOURCE_2 \
    LOGISTICS_RTSP_SOURCE_3 LOGISTICS_RTSP_SOURCE_4 \
    LOGISTICS_RTSP_RELAY_USER LOGISTICS_RTSP_RELAY_PASSWORD
}
trap cleanup_rtsp_env EXIT HUP INT TERM

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
cleanup_rtsp_env
trap - EXIT HUP INT TERM
unset -f cleanup_rtsp_env
test "${setup_status}" -eq 0
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

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_MQTT_HOST='192.168.0.10'
export LOGISTICS_UPLOAD_TOKEN='중앙서버와-동일한-토큰'
export LOGISTICS_DEVICE_ID='PI-VISION-01'
export LOGISTICS_DEVICE_IP='192.168.0.21'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-vision-node.sh
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

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_DEVICE_ID='PI-INPUT-01'
export LOGISTICS_DEVICE_IP='192.168.0.22'
export LOGISTICS_UART_DEVICE='/dev/vedauart'
./deploy/scripts/setup-input-node.sh
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

현재 검사 스크립트는 개발용 MQTT `1883`과 HTTP `8080`을 확인합니다. TLS 전환 후에는
[Mosquitto TLS 가이드](../mosquitto/README.md)에 있는 `mosquitto_sub`, `mosquitto_pub`, `openssl s_client`
검증을 사용해야 합니다.

## 선택 환경 변수

| 변수 | 용도 |
| --- | --- |
| `LOGISTICS_CONFIG_PATH` | 생성·사용할 INI 경로 |
| `LOGISTICS_BUILD_DIR` | CMake 빌드 경로 |
| `LOGISTICS_RUNTIME_DIR` | 런타임 데이터 경로 |
| `LOGISTICS_NODE_NAME` | 장치 등록 이름 |
| `LOGISTICS_FORCE_CONFIG=1` | 기존 INI 덮어쓰기 |
| `LOGISTICS_INSTALL_DEPENDENCIES=1` | apt 의존성 설치 |
| `LOGISTICS_INSTALL_OPENCV=1` | OpenCV 4.10.0 소스 설치 |

토큰과 비밀번호를 스크립트 파일에 직접 기록하지 않습니다.
