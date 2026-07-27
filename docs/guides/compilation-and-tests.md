# 빌드 및 테스트

모든 C++ 타깃은 C++20을 사용하며 경고를 오류로 처리합니다. 서로 다른 기기에서 실행할 구성 요소는 각 기기에서
별도 빌드 디렉터리를 사용하는 것을 권장합니다.

## 중앙서버

```sh
cmake -S . -B build-central -G Ninja \
  -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
  -DLOGISTICS_BUILD_CENTRAL_SERVER=ON \
  -DLOGISTICS_BUILD_DEVICE_NODES=OFF \
  -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON

cmake --build build-central
ctest --test-dir build-central --output-on-failure
```

실행 파일:

```text
build-central/central-server-rpi/logistics_central_server
```

## Vision 노드

OpenCV 4.10.0이 설치된 Vision Raspberry Pi에서 실행합니다.

```sh
cmake -S . -B build-vision -G Ninja \
  -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
  -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
  -DLOGISTICS_BUILD_DEVICE_NODES=ON \
  -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON

cmake --build build-vision --target logistics_vision_node
ctest --test-dir build-vision --output-on-failure
```

실행 파일:

```text
build-vision/device-rpi/logistics_vision_node
```

## 공통 Device 노드

현재 생성되는 실행 타깃은 다음과 같습니다.

- `logistics_input_node`
- `logistics_vision_node`
- `logistics_sorting_node`
- `logistics_linetracer_node`

Input, Sorting, Line Tracer의 `main` 구현은 공통 `NodeRuntime`을 사용합니다. 장치별 MQTT↔UART 동작이 실제로
구현된 브랜치와 합쳐지기 전에는 등록·heartbeat·공통 명령 처리 범위만 기대해야 합니다. Gripper 전용 Raspberry Pi
실행 타깃은 아직 없습니다.

## Control Center

Windows와 Qt 설치 방법은 [Windows Qt 설치](../setup/windows-control-center.md)를 참고합니다.

```sh
cmake -S control-center -B build-control-center -G Ninja
cmake --build build-control-center
ctest --test-dir build-control-center --output-on-failure
```

Qt 6.10 이상의 `Mqtt`, `Multimedia`, `Network`, `Widgets` 모듈이 필요합니다.

## 통신 없는 코어 테스트

libmosquitto를 사용할 수 없는 환경에서는 MQTT 전송 계층을 끄고 계약과 코어 로직을 검사할 수 있습니다.

```sh
cmake -S . -B build-core -G Ninja \
  -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
  -DLOGISTICS_BUILD_CENTRAL_SERVER=ON \
  -DLOGISTICS_BUILD_DEVICE_NODES=OFF \
  -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

이 구성에는 실제 중앙서버 실행 파일과 MQTT smoke 타깃이 만들어지지 않습니다.

## 포맷

변경한 C/C++ 파일은 저장소의 clang-format 규칙을 적용합니다.

```sh
clang-format --dry-run --Werror path/to/changed.cpp
clang-format -i path/to/changed.cpp
```

문서 변경 후에는 [문서 링크 검사](#문서-검증)도 수행합니다.

## 문서 검증

Markdown 링크는 저장소 루트를 기준으로 실제 파일이 존재해야 합니다. GitHub에서 루트 README와
`docs/README.md`를 열어 상대 링크가 올바른지 확인합니다. 비밀번호, 토큰, 개인키가 staged diff에 포함되지 않았는지도
반드시 확인합니다.
