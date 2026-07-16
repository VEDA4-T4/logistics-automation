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

[http]
image_base_url=http://127.0.0.1:8080/
```

연결되면 QoS 1로 중앙 서버 및 해당 클라이언트 토픽을 구독합니다. 연결이 끊기면
`reconnect_interval_ms` 간격으로 재연결하며, 연결 및 오류 상태는 중앙관제 상태 표시줄에 나타납니다.
상품 메시지의 상대 이미지 경로는 `http/image_base_url`을 기준으로 조회합니다. 이미지 바이너리는 MQTT로
전송하지 않습니다.

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
