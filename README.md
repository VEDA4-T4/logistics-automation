# Logistics Automation

Qt 관제 시스템, 중앙 Raspberry Pi 서버, 공정별 Raspberry Pi 노드와 STM32 제어기를 MQTT·HTTP(S)·UART로
연결하는 물류 자동화 프로젝트입니다.

## 구성

```text
Control Center (Qt/Windows)
          │ MQTT
          ▼
Mosquitto ── Central Server (Raspberry Pi, SQLite, HTTP upload)
          │ MQTT
          ▼
Input → Vision → Gripper → Sorting → Line Tracer
          │
          └─ Device Raspberry Pi ↔ UART ↔ STM32
```

- `control-center/`: Qt 관제 화면, 공정 제어, RTSP/ONVIF 영상
- `central-server-rpi/`: MQTT 라우팅, 공정 상태 머신, SQLite, 이미지·로그 HTTP 업로드
- `device-rpi/`: 공정별 MQTT 노드와 STM32 UART 브리지
- `stm32/`: Input, Gripper, Sorting, Line Tracer 펌웨어
- `shared/`: MQTT·HTTP·UART 통신 계약
- `deploy/`: 설치 스크립트, Mosquitto, systemd 운영 자료

상세한 책임과 의존성 규칙은 [프로젝트 구조](docs/architecture/project-structure.md)를 참고하세요.

## 빠른 시작

Ubuntu/Raspberry Pi에서 중앙서버를 준비합니다. MQTT 비밀번호는 shell history에 남기지 않도록 prompt로 입력합니다.

```bash
read -rsp 'central-server MQTT password: ' LOGISTICS_MQTT_PASSWORD; printf '\n'
export LOGISTICS_MQTT_PASSWORD
export LOGISTICS_UPLOAD_TOKEN='충분히-긴-임의의-토큰'
export LOGISTICS_MQTT_HOST='mqtt.logistics.local'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-central-server.sh
unset LOGISTICS_MQTT_PASSWORD
```

Vision Raspberry Pi에서는 중앙서버의 실제 LAN 주소를 사용합니다.

```bash
read -rsp 'PI-VISION-01 MQTT password: ' LOGISTICS_MQTT_PASSWORD; printf '\n'
export LOGISTICS_MQTT_PASSWORD
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_MQTT_HOST='mqtt.logistics.local'
export LOGISTICS_UPLOAD_TOKEN='중앙서버와-동일한-토큰'
export LOGISTICS_DEVICE_ID='PI-VISION-01'
export LOGISTICS_INSTALL_DEPENDENCIES=1
export LOGISTICS_INSTALL_OPENCV=1
./deploy/scripts/setup-vision-node.sh
unset LOGISTICS_MQTT_PASSWORD
```

생성된 설정은 Git에서 제외되는 `runtime/` 아래에 보관됩니다. 전체 설치와 실행 순서는
[통합 실행 가이드](docs/guides/integration-runbook.md)를 따르세요.

## 문서

| 목적 | 문서 |
| --- | --- |
| 구성 요소별 안내 | [Control Center](control-center/README.md) · [Central Server](central-server-rpi/README.md) · [Device Nodes](device-rpi/README.md) |
| 처음 설치 | [Ubuntu/Raspberry Pi 설치](docs/setup/ubuntu-rpi.md) · [Windows Qt 설치](docs/setup/windows-control-center.md) |
| 빌드와 테스트 | [빌드 및 테스트](docs/guides/compilation-and-tests.md) |
| INI와 장치 주소 | [런타임 설정](docs/guides/runtime-configuration.md) |
| 전체 시스템 실행 | [통합 실행 가이드](docs/guides/integration-runbook.md) |
| 운영 및 장애 대응 | [운영 점검과 문제 해결](docs/guides/operations-troubleshooting.md) |
| Mosquitto 인증·ACL·TLS | [Mosquitto 보안 및 TLS](deploy/mosquitto/README.md) |
| MQTT·HTTP·UART 규격 | [통신 계약](docs/guides/communication-contracts.md) |
| STM32 개발 | [STM32 안내](stm32/README.md) |

전체 문서 목록은 [문서 허브](docs/README.md)에서 확인할 수 있습니다.

## 기본 빌드

Qt 관제 시스템을 제외한 호스트 코드는 다음 명령으로 빌드합니다.

```sh
cmake -S . -B build -G Ninja \
  -DLOGISTICS_BUILD_CONTROL_CENTER=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

플랫폼별 의존성과 개별 타깃 명령은 [빌드 및 테스트](docs/guides/compilation-and-tests.md)에 정리되어 있습니다.

## 보안 주의

- 실제 `.ini`, 비밀번호, 업로드 토큰, 개인키는 커밋하지 않습니다.
- 다른 기기에서 중앙서버에 접속할 때 `127.0.0.1` 대신 중앙서버의 LAN 주소 또는 DNS 이름을 사용합니다.
- MQTT 클라이언트는 CA 검증을 사용하는 TLS `8883`을 지원합니다. 설정의 `host`는 broker 서버 인증서 SAN에 포함된
  DNS 이름 또는 IP여야 합니다.
