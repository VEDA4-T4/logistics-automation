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

## 운영 로그

- 메인 화면은 최신 로그 500건으로 시작합니다.
- `http/bearer_token`이 설정되어 있으면 표의 맨 아래에서 계속 아래로 스크롤할 때 중앙서버의 과거 로그를
  cursor 기반으로 500건씩 추가 조회합니다.
- 새 페이지는 이미 표시한 행을 교체하지 않고 표 아래에 이어 붙이며, 한 화면 세션에서는 최대 5,000건까지
  누적합니다.
- MQTT 실시간 로그는 짧은 주기로 묶어 모델에 증분 반영하고 `messageId`가 같은 항목은 중복 표시하지 않습니다.
- 과거 로그를 보는 동안 새 로그가 도착해도 스크롤 위치를 유지하며, 최신 위치로 돌아오면 누적된 신규 로그를
  확인할 수 있습니다.
- 등급·검색어·미확인 필터를 지원하고, 한 번 클릭하면 확인 처리하며 두 번 클릭하면 상세 내용을 표시합니다.

상품 정보의 비어 있는 값은 `데이터 없음`을 황색 안내 글씨로 표시하며, 실제 값은 어두운 배경에서 읽을 수 있는
밝은 글씨로 표시합니다.

## 실행 전 확인

- Mosquitto가 먼저 실행 중이어야 합니다.
- 중앙서버의 `routing/qt_client_id`와 Control Center `mqtt/client_id`가 같아야 합니다.
- `dashboard/*_device_id`는 각 장치 MQTT `sourceId`와 같아야 합니다.
- 상대 이미지 경로는 `http/image_base_url`을 기준으로 다운로드됩니다.

전체 순서는 [통합 실행 가이드](../docs/guides/integration-runbook.md)를 따릅니다.

## MQTT TLS

Control Center는 CA 인증서로 broker 인증서를 검증하는 MQTT TLS 연결을 지원합니다. `[mqtt]`에 broker 인증서의
SAN과 일치하는 `host`, TLS listener의 `port`, `tls_enabled=true`, PEM 형식의 `ca_certificate` 경로를 설정합니다.
연결에는 TLS 1.2 이상과 peer 검증이 적용됩니다.

`username`과 `password`는 broker가 요구하는 MQTT 계정 인증에 사용하며 TLS 설정과 별개입니다. TLS를 끄면
`connectToHost()`를 사용하는 평문 연결이므로 운영 환경에서는 사용하지 않습니다. 현재 client 인증서와 private key를
사용하는 mTLS 및 사용자 지정 TLS version/cipher 설정은 지원하지 않습니다. broker 인증서와 CA 준비 절차는
[Mosquitto TLS 설정](../deploy/mosquitto/README.md#4-내부-ca와-서버-인증서)을 참고하세요.
