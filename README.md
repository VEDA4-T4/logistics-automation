# Logistics Automation

AI CCTV, Qt 중앙관제, 중앙 서버 Raspberry Pi, 장치 Raspberry Pi, STM32와 물류 설비를 통합하는 스마트 물류
자동화 프로젝트입니다.

## Repository layout

- `control-center/`: Qt 기반 중앙관제 및 RTSP 화면
- `central-server-rpi/`: MQTT 메시지 처리, 장치/작업 관리, SQLite 저장
- `device-rpi/`: 공정별 Raspberry Pi 노드와 MQTT-UART bridge
- `stm32/`: 컨베이어, 회전, 분류, 라인트레이서 펌웨어
- `shared/`: MQTT/UART 계약과 공통 도메인 타입
- `deploy/`: Mosquitto 및 systemd 배포 설정
- `docs/`: 아키텍처와 개발 문서

상세한 디렉터리 책임은 [docs/architecture/project-structure.md](docs/architecture/project-structure.md)를 참고하세요.

## Build

기본 빌드는 Qt가 필요 없는 중앙 서버와 장치 노드 골격을 빌드합니다.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Qt 6가 설치된 환경에서 중앙관제를 함께 빌드하려면 다음 옵션을 사용합니다.

```sh
cmake -S . -B build -DLOGISTICS_BUILD_CONTROL_CENTER=ON
```

STM32 펌웨어는 호스트 빌드에서 제외되며 STM32CubeIDE 또는 ARM toolchain으로 각 controller를 빌드합니다.
