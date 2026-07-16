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

### 중앙 서버 MQTT 설정

중앙 서버는 외부 Mosquitto broker에 `libmosquitto` 클라이언트로 연결합니다. Ubuntu/Raspberry Pi 빌드 환경에
개발 패키지를 설치한 뒤 예제 설정을 복사해 broker 주소와 인증 정보를 입력합니다.

```sh
sudo apt-get update
sudo apt-get install -y libmosquitto-dev
cp central-server-rpi/config/server.ini.example central-server-rpi/config/server.ini
cmake -S . -B build
cmake --build build
./build/central-server-rpi/logistics_central_server central-server-rpi/config/server.ini
```

설정 경로는 실행 파일의 첫 번째 인수 또는 `LOGISTICS_CENTRAL_SERVER_CONFIG` 환경 변수로 지정할 수 있습니다.
연결이 끊기면 설정한 최소·최대 지연 사이에서 지수 backoff로 자동 재연결하고, 연결이 복구될 때 서버의 필수
topic을 다시 구독합니다. 연결 오류는 표준 오류로 기록되어 systemd journal에서 확인할 수 있습니다.
등록 장치 목록은 `[device_registry] path`의 JSON 파일에 보존됩니다. 중앙 서버 재시작 시 기존 장치는
`OFFLINE`으로 복원되고 retained 상태나 다음 등록 메시지가 도착하면 같은 장치 ID의 상태가 갱신됩니다.
상대 경로는 `server.ini`가 있는 디렉터리를 기준으로 해석됩니다. 이 파일 저장소는 SQLite 장치 저장소가
실행 경로에 연결되기 전까지 사용하는 임시 어댑터입니다.

Last Will payload는 MQTT CONNECT 전에 broker에 등록되므로 비정상 단절 순간에 장치가 payload의 timestamp를
다시 쓸 수 없습니다. 장치 목록은 payload의 `lastReportedTimestamp`와 별도로 마지막 heartbeat 발생 시각
`lastHeartbeatTimestamp`, 중앙 서버가 Will을 받은 실제 시각 `lastSeenTimestamp`와 `disconnectedAt`을
보존합니다. 정상 종료 메시지의 timestamp는 종료 직전에 새로 생성됩니다.

`libmosquitto` 없이 설정 파서와 MQTT 상태 로직만 빌드·테스트하려면
`-DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=OFF`를 지정합니다.

### 장치 Raspberry Pi MQTT 설정

장치 노드는 `device-rpi/config/node.ini.example`을 복사해 고유 `device_id`, `node_name`, 장치 IP 주소,
broker 주소와 인증 정보를 설정합니다. 중앙 서버와 함께 기본 빌드하면 같은 `libmosquitto` 라이브러리를
사용하도록 장치 MQTT runtime도 활성화됩니다.

```sh
cp device-rpi/config/node.ini.example device-rpi/config/node.ini
cmake -S . -B build
cmake --build build
./build/device-rpi/logistics_vision_node device-rpi/config/node.ini
```

설정 경로는 첫 번째 실행 인수 또는 `LOGISTICS_DEVICE_CONFIG` 환경 변수로 지정할 수 있습니다. 연결되면
`device/{deviceId}/command`와 `system/broadcast/command`를 구독하고, retained `ONLINE` 상태와 장치 등록을
공통 JSON codec으로 발행합니다. 이어서 heartbeat를 5초마다 `device/{deviceId}/heartbeat`로 발행합니다.
정상 종료 시에는 retained `OFFLINE` 상태를 먼저 발행하고 연결을 종료합니다. 비정상 연결 종료 시에는 같은
`OFFLINE` 상태가 Last Will로 발행되며, 재연결하면 retained `ONLINE` 상태가 이를 덮어쓰고 장치를 다시 등록합니다.

중앙 서버의 공통 codec → MQTT client → libmosquitto 발행 경로는 별도 smoke 실행 파일로 확인할 수 있습니다.
Raspberry Pi에서 해당 장치 노드를 실행하거나 `device/{deviceId}/command`를 구독한 뒤 Ubuntu에서 실행합니다.

```sh
./build/central-server-rpi/central_server_mqtt_smoke \
  central-server-rpi/config/server.ini PI-TEST
```

성공하면 QoS 1 `STATUS_REQUEST`가 `device/PI-TEST/command`로 한 번 발행됩니다. 이 도구는 실행 중인 중앙
서버의 client ID와 충돌하지 않도록 매 실행마다 별도 smoke client ID를 사용합니다.

### 중앙관제 MQTT 설정

`control-center/config/control-centor.ini.example`을 `control-centor.ini`로 복사한 뒤 브로커 값을 입력합니다.
실제 설정 파일은 접속 정보를 포함할 수 있으므로 Git에서 제외되며, 빌드할 때 실행 파일의 `config` 폴더로
자동 복사됩니다. 다른 위치의 설정을 사용하려면 `LOGISTICS_CONTROL_CENTER_CONFIG` 환경 변수에 INI 파일의
절대 경로를 지정합니다.

```ini
[mqtt]
host=127.0.0.1
port=1883
client_id=control-center
username=
password=
reconnect_interval_ms=3000
keep_alive_seconds=30
```

연결되면 QoS 1로 중앙 서버 및 해당 클라이언트 토픽을 구독합니다. 연결이 끊기면
`reconnect_interval_ms` 간격으로 재연결하며, 연결 및 오류 상태는 중앙관제 상태 표시줄에 나타납니다.

### Qt MQTT 모듈 직접 빌드 (Windows)

중앙관제의 MQTT 클라이언트는 Qt와 버전 및 toolchain이 같은 Qt MQTT 모듈이 필요합니다. 아래 명령은
Qt 6.11.1, MinGW 13.1 64-bit, CMake 3.30, Ninja 환경을 기준으로 합니다. Qt 버전을 변경하면
`QtRoot`, 소스 태그와 도구 경로를 함께 변경해야 합니다.

```powershell
$QtRoot = "C:/Qt/6.11.1/mingw_64"
$Work = Join-Path $env:TEMP "qtmqtt-6.11.1"
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\CMake_64\bin;$QtRoot/bin;$env:PATH"

git clone --branch v6.11.1 --depth 1 https://github.com/qt/qtmqtt.git "$Work/src"
New-Item -ItemType Directory -Path "$Work/build" -Force | Out-Null

Push-Location "$Work/build"
& "$QtRoot/bin/qt-configure-module.bat" "$Work/src" -cmake-generator Ninja -- `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_INSTALL_PREFIX:PATH=$QtRoot" `
    "-DQT_BUILD_TESTS=OFF" `
    "-DQT_BUILD_EXAMPLES=OFF"
Pop-Location

cmake --build "$Work/build" --parallel
cmake --install "$Work/build"

Test-Path "$QtRoot/lib/cmake/Qt6Mqtt/Qt6MqttConfig.cmake"
Remove-Item -LiteralPath $Work -Recurse -Force
```

`cmake --install`에서 권한 오류가 발생하면 관리자 PowerShell에서 설치 명령만 다시 실행합니다. 설치 확인
명령은 `True`를 반환해야 합니다. 프로젝트에서는 다음과 같이 모듈을 탐색하고 링크합니다.

```cmake
find_package(Qt6 REQUIRED COMPONENTS Mqtt)
target_link_libraries(logistics_control_center PRIVATE Qt6::Mqtt)
```

Qt Creator 밖에서 실행 파일을 직접 실행하거나 배포할 때는 Qt DLL을 실행 파일 옆에 배치해야 합니다.

```powershell
& "$QtRoot/bin/windeployqt.exe" --release path\to\logistics_control_center.exe
```

Qt MQTT는 상용 라이선스 또는 GPLv3로 제공되므로 배포 전에 프로젝트 라이선스와의 호환성을 확인합니다.

STM32 펌웨어는 호스트 빌드에서 제외되며 STM32CubeIDE 또는 ARM toolchain으로 각 controller를 빌드합니다.
