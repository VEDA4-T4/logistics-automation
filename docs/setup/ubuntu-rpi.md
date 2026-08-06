# Ubuntu/Raspberry Pi 설치

중앙서버와 장치 노드는 Ubuntu 또는 Raspberry Pi OS 계열 Linux에서 빌드합니다. 명령은 저장소 루트에서
실행합니다.

## 공통 도구

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config git \
  nlohmann-json3-dev libssl-dev
```

## 중앙서버

```sh
sudo apt install -y libmosquitto-dev libsqlite3-dev libmicrohttpd-dev
```

Mosquitto 브로커를 같은 기기에서 운영한다면 다음 패키지도 설치합니다.

```sh
sudo apt install -y mosquitto mosquitto-clients
```

`libmosquitto-dev`는 애플리케이션을 빌드하기 위한 라이브러리이고, `mosquitto`는 실제 브로커 서비스입니다.
둘은 서로 다른 역할입니다.

## Device Raspberry Pi

```sh
sudo apt install -y libmosquitto-dev libcurl4-openssl-dev
```

Vision 노드는 OpenCV `4.10.0`의 `core`, `highgui`, `imgcodecs`, `imgproc`, `objdetect`, `videoio` 구성 요소를
정확히 요구합니다. 시스템에 다른 버전만 있다면 제공된 설치 스크립트를 사용합니다.

```sh
export LOGISTICS_INSTALL_OPENCV=1
export LOGISTICS_INSTALL_DEPENDENCIES=1
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_UPLOAD_TOKEN='중앙서버와-동일한-토큰'
./deploy/scripts/setup-vision-node.sh
```

소스 빌드는 시간이 오래 걸릴 수 있습니다. 설치 후 다음 결과가 `4.10.0`인지 확인합니다.

```sh
pkg-config --modversion opencv4
```

## UART 장치

Raspberry Pi와 STM32를 커널 serdev 드라이버로 연결할 경우
[VEDAUART 드라이버 가이드](../../device-rpi/kernel/vedauart/README.md)를 따릅니다. 먼저 MQTT만 검증할 때는
UART 장치가 없는 상태에서도 공통 노드의 등록·heartbeat 동작을 확인할 수 있지만 실제 공정 제어는 할 수 없습니다.

## 설치 확인

```sh
cmake --version
ninja --version
pkg-config --modversion libmosquitto
pkg-config --modversion sqlite3
```

다음 단계는 [빌드 및 테스트](../guides/compilation-and-tests.md)와
[통합 실행 가이드](../guides/integration-runbook.md)입니다.
