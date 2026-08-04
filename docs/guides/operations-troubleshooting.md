# 운영 점검과 문제 해결

## 기본 점검 순서

문제가 발생하면 네 계층을 순서대로 확인합니다.

1. TCP 포트 접근
2. MQTT/HTTP 인증과 TLS
3. 메시지 계약과 공정 상태
4. 파일·DB·카메라·UART 같은 로컬 자원

중앙서버 로그:

```sh
journalctl -u logistics-central-server -n 200 --no-pager
```

Mosquitto 로그:

```sh
systemctl status mosquitto --no-pager
journalctl -u mosquitto -n 200 --no-pager
```

## 포트는 열리지만 MQTT 연결이 안 됨

```sh
nc -vz mqtt.logistics.local 8883
mosquitto_sub -h mqtt.logistics.local -p 8883 \
  --cafile /etc/logistics/tls/ca.crt \
  -u PI-VISION-01 -P '해당-장치에-발급한-비밀번호' \
  -t 'device/PI-VISION-01/status' -d
```

TCP 성공 후 MQTT가 실패하면 사용자/비밀번호, ACL, client ID 중복, CA 경로, 인증서 SAN과 host 일치를 확인합니다.
TLS listener `8883`에는 `--cafile` 없이 평문 클라이언트로 접속할 수 없습니다. 마이그레이션용 `1883`도 익명 접근은
거부하므로 사용자명과 비밀번호가 필요합니다.

## `nlohmann/json.hpp was not found`

Ubuntu/Raspberry Pi:

```sh
sudo apt update
sudo apt install nlohmann-json3-dev
```

Windows:

```powershell
cd C:\programming\workspace\logistics-automation
$VcpkgExe = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg\vcpkg.exe"
& $VcpkgExe install --triplet x64-windows
```

Qt Creator가 이전 실패를 캐시했다면 `Build > Clear CMake Configuration` 후 다시 구성합니다.

## `Unknown option: --headless` 또는 `--camera`

- `--camera`는 지원하지 않습니다.
- 현재 `main`의 Vision은 `--headless`를 지원합니다.
- `--headless`도 알 수 없다고 나오면 이전 브랜치의 실행 파일을 사용 중일 가능성이 높습니다.

```sh
git branch --show-current
cmake --build build-vision --target logistics_vision_node --clean-first
./build-vision/device-rpi/logistics_vision_node --help
```

## `qt.qpa.xcb: could not connect to display`

SSH 또는 모니터가 없는 Raspberry Pi에서는:

```sh
./build-vision/device-rpi/logistics_vision_node \
  --headless \
  --config runtime/vision-node/vision-node.ini
```

headless 옵션이 없던 오래된 실행 파일이면 먼저 다시 빌드합니다.

## 이미지 다운로드 HTTP 500

Control Center의 TCP 연결 성공과 이미지 다운로드 성공은 별개입니다. 중앙서버에서 다음을 확인합니다.

```sh
curl -v http://192.168.0.10:8080/uploads/images/example.jpg
find runtime/central-server/uploads/images -maxdepth 1 -type f
```

- MQTT `imagePath`가 실제 업로드 응답 경로인지
- 파일이 `upload_root/images`에 존재하는지
- 중앙서버 프로세스가 파일을 읽을 권한이 있는지
- Control Center `image_base_url`이 중앙서버 주소인지
- HTTP와 HTTPS 포트를 혼동하지 않았는지 확인합니다.

404는 파일이 없다는 뜻이고 500은 서버가 요청을 처리하는 중 내부 오류가 발생했다는 뜻입니다.

## 이미지가 너무 빨리 실패로 바뀜

Vision의 바코드 인식은 여러 프레임을 기다리도록 구현되어 있습니다. 오래된 실행 파일이라면 최신 `main`에서 다시
빌드합니다. Control Center는 실패 이벤트가 들어와도 마지막 성공 이미지를 유지하고 다음 성공 이미지가 올 때
교체하는 것이 기본 동작입니다.

## `BARCODE_DETECTED requires barcode`

`recognitionStatus=SUCCESS`인 메시지는 비어 있지 않은 `barcode`를 포함해야 합니다. 실패라면
`recognitionStatus=FAILED`와 실패 메시지를 보내야 합니다. 성공 상태에 빈 바코드를 넣으면 중앙서버 저장 검증에서
거부됩니다.

## `CHECK constraint failed: process_state`

장치가 보내는 `currentState`와 DB의 정규화된 `process_state`를 혼동하지 않습니다. DB가 허용하는 상위 상태는
`IDLE`, `RUNNING`, `STOPPED`, `ERROR`, `ESTOP`, `RECOVERY`, `UNKNOWN`입니다. 새로운 상위 상태가 필요하면 코드,
계약, DB migration을 함께 변경해야 합니다.

## Mosquitto 서비스 시작 실패

```sh
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v
```

서비스가 이미 포트를 사용 중이면 먼저 중지한 뒤 foreground 검증을 수행합니다.

```sh
sudo systemctl stop mosquitto
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v
# 확인 후 Ctrl-C
sudo systemctl start mosquitto
```

흔한 원인은 중복 listener, 읽을 수 없는 개인키, 잘못된 인증서 경로, 다른 프로세스의 포트 점유입니다.

## 테스트가 `Subprocess aborted`로 끝남

실패한 테스트만 자세히 실행합니다.

```sh
ctest --test-dir build-central -R central_server_mqtt_handler_test --output-on-failure
ctest --test-dir build-central -R central_storage_test --output-on-failure
```

기존 DB 대신 테스트용 임시 DB를 사용하는지, migration 경로가 실제 저장소의
`central-server-rpi/db/migrations`를 가리키는지 확인합니다.
