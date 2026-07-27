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

## 연결 검사

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
