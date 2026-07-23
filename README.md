# Logistics Automation

AI CCTV, Qt 중앙관제, 중앙 서버 Raspberry Pi, 장치 Raspberry Pi, STM32와 물류 설비를 통합하는 스마트 물류
자동화 프로젝트입니다.

## Repository layout

- `control-center/`: Qt 기반 중앙관제 및 RTSP 화면
- `central-server-rpi/`: MQTT 메시지 처리, 장치/작업 관리, SQLite 저장
- `device-rpi/`: 공정별 Raspberry Pi 노드와 MQTT-UART bridge
- `stm32/`: 컨베이어, 그리퍼, 분류, 라인트레이서 펌웨어
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

### Windows Qt 빌드 의존성 (vcpkg)

중앙관제도 `shared`의 MQTT JSON 계약을 사용하므로 `nlohmann-json`이 필요합니다. Windows에서는 저장소
루트의 `vcpkg.json` manifest를 사용해 설치합니다. Visual Studio 2022에 포함된 vcpkg가 PATH에 없다면
다음과 같이 실행합니다.

```powershell
cd C:\programming\workspace\logistics-automation

$VcpkgExe = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg\vcpkg.exe"
& $VcpkgExe install --triplet x64-windows
```

설치 결과는 `vcpkg_installed/x64-windows`에 생성되며 Git에는 포함되지 않습니다. 프로젝트 CMake는 이
경로의 `nlohmann_json` 패키지를 자동으로 탐색하므로 Qt Creator에 `CMAKE_TOOLCHAIN_FILE`이나
`nlohmann_json_DIR`를 별도로 지정할 필요가 없습니다.

Qt Creator에서는 `control-center/CMakeLists.txt`를 열고 `Desktop Qt 6.11.1 MinGW 64-bit`와 같이 설치된
Qt 버전에 맞는 kit를 선택합니다. 기존에 의존성 탐색이 실패한 빌드 디렉터리는 실패 결과가 캐시되어 있을
수 있으므로 다음 순서로 다시 구성합니다.

1. **Build > Clear CMake Configuration**
2. **Build > Run CMake**
3. **Build > Build Project**

명령줄에서 동일한 구성을 확인하려면 Qt의 CMake와 기존 Qt Creator 빌드 디렉터리를 사용할 수 있습니다.

```powershell
$QtCMake = "C:\Qt\Tools\CMake_64\bin\cmake.exe"
$BuildDir = "control-center\build\Desktop_Qt_6_11_1_MinGW_64_bit_Debug"

& $QtCMake -S control-center -B $BuildDir
& $QtCMake --build $BuildDir --parallel 4
```

Ubuntu/Raspberry Pi에서는 vcpkg 대신 시스템 패키지를 설치합니다.

```sh
sudo apt update
sudo apt install nlohmann-json3-dev
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

[dashboard]
input_device_id=PI-INPUT-01
vision_device_id=PI-VISION-01
gripper_device_id=PI-GRIPPER-01
sorting_device_id=PI-SORTING-01
linetracer_device_id=PI-LT-01

[http]
image_base_url=http://127.0.0.1:8080/

[rtsp]
channel_count=4
reconnect_interval_ms=3000
low_latency=true
network_timeout_ms=3000
probe_size_bytes=32768
onvif_metadata_enabled=true
onvif_log_payload=false
metadata_stale_timeout_ms=1500
```

`rtsp/low_latency=true`이면 각 RTSP 소스를 열기 전에 Qt Multimedia의 저지연 스트리밍 모드를 적용합니다.
재생 버퍼가 줄어드는 대신 패킷 손실이 화면에 더 쉽게 드러날 수 있으므로, 지연보다 부드러운 재생이 중요하면
`false`로 변경합니다. `rtsp/network_timeout_ms`는 소켓 입출력이 멈췄을 때 기존 재연결 흐름으로 전환할
때까지 기다리는 시간을 지정합니다. `probe_size_bytes`는 재생 전 스트림 분석량이며 기본값 32768은
RTSP 시작 대기를 줄이기 위한 값입니다. 스트림 정보 인식에 실패하면 값을 늘립니다.

`rtsp/onvif_metadata_enabled=true`이면 각 채널 URL의 RTSP `application` 트랙에서
`vnd.onvif.metadata` XML을 구독하고 객체의 바운딩 박스, 분류명, 신뢰도를 영상 위에 표시합니다.
메타데이터가 별도 프로파일에 있다면 `channel_N_metadata_url`을 지정하고, 생략하면 `channel_N_url`을
그대로 사용합니다. 현재 비압축 ONVIF XML을 지원하며 GZIP/EXI 메타데이터는 지원하지 않습니다.
`metadata_stale_timeout_ms` 동안 새 프레임이 없으면 오래된 박스를 자동으로 지웁니다.
`onvif_log_payload=true`이면 Qt Creator의 **Application Output**에 원본 XML과 파싱된 객체
ID·클래스·신뢰도·바운딩 박스를 채널별 `[ONVIF][CH N]` 형식으로 출력합니다. 이 로그는 프레임마다
발생하여 UI 렌더링을 지연시킬 수 있으므로 기본값은 `false`이며 진단할 때만 활성화합니다.

`dashboard`의 장치 ID는 각 장치가 MQTT envelope의 `sourceId`로 보내는 값과 같아야 합니다. 각 공정은
자신의 `jobId`를 독립적으로 표시하므로 서로 다른 상품을 동시에 처리할 수 있습니다.

### 상품 인식 및 이송 전제

- 바코드는 상품 윗면에 부착되며 Vision은 상단 프레임에서 바코드를 인식합니다.
- 상품을 회전하며 여러 면을 탐색하지 않습니다.
- 그리퍼는 상품을 집어 컨베이어 사이로 옮기고 내려놓는 역할만 담당합니다.
- 그리퍼는 상품 회전, 방향 보정 또는 바코드 탐색을 수행하지 않습니다.

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
