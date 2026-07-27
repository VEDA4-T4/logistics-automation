# Control Center

Qt 6 기반 관제 애플리케이션입니다. MQTT 공정 제어·상태·이벤트, 작업 이력, RTSP 영상과 ONVIF metadata overlay를
표시합니다.

## 요구 사항

- Qt 6.10 이상
- Qt `Mqtt`, `Multimedia`, `Network`, `Widgets`
- CMake 3.20 이상
- C++20 MinGW 또는 호환 compiler
- `nlohmann-json`

Windows 설치는 [Windows Qt 설치](../docs/setup/windows-control-center.md)를 참고합니다.

## 빌드

```powershell
cmake -S control-center -B build-control-center -G Ninja
cmake --build build-control-center --parallel 4
ctest --test-dir build-control-center --output-on-failure
```

Qt Creator에서는 이 폴더의 `CMakeLists.txt`를 직접 열고 Qt 버전과 일치하는 64-bit kit를 선택합니다.

## 설정

```powershell
Copy-Item control-center\config\control-centor.ini.example `
  control-center\config\control-centor.ini
```

MQTT `host`에는 broker의 주소, `http/image_base_url`에는 중앙서버의 주소를 넣습니다. 다른 PC에서 실행할 때
`127.0.0.1`을 사용하면 해당 Windows PC 자신에게 접속하므로 주의합니다.

설정 키와 장치 ID 매핑은 [런타임 설정](../docs/guides/runtime-configuration.md)을 참고하세요.

## 실행 전 확인

- Mosquitto가 먼저 실행 중이어야 합니다.
- 중앙서버의 `routing/qt_client_id`와 Control Center `mqtt/client_id`가 같아야 합니다.
- `dashboard/*_device_id`는 각 장치 MQTT `sourceId`와 같아야 합니다.
- 상대 이미지 경로는 `http/image_base_url`을 기준으로 다운로드됩니다.

전체 순서는 [통합 실행 가이드](../docs/guides/integration-runbook.md)를 따릅니다.

## MQTT TLS

현재 구현은 사용자/비밀번호를 지원하지만 암호화 연결은 아직 적용하지 않았습니다. broker를 TLS 전용으로 바꾸기 전에
[Mosquitto TLS의 Qt 구현 항목](../deploy/mosquitto/README.md#qt-control-center)을 완료해야 합니다.
