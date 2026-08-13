# 런타임 설정

실제 INI에는 IP 주소, 사용자 이름, 비밀번호와 토큰이 들어갈 수 있으므로 Git에 커밋하지 않습니다. 예제 파일을
`runtime/` 아래로 복사하고 해당 기기에서만 관리합니다.

## 주소 원칙

`127.0.0.1`은 “현재 실행 중인 기기 자신”입니다. 구성 요소가 서로 다른 기기에서 실행되면 중앙서버 또는 브로커의
LAN 주소/DNS 이름을 사용해야 합니다.

| 실행 위치 | MQTT `host` | HTTP 이미지 주소 |
| --- | --- | --- |
| 중앙서버와 Mosquitto가 같은 기기 | `127.0.0.1` | 해당 없음 |
| Vision Raspberry Pi | 브로커 LAN 주소 | 중앙서버 LAN 주소 |
| Control Center PC | 브로커 LAN 주소 | 중앙서버 LAN 주소 |

예시:

```text
Broker/Central Server  192.168.0.10
Vision Pi              192.168.0.21
Control Center PC      192.168.0.30
```

## 중앙서버

기준 파일: `central-server-rpi/config/server.ini.example`

```sh
mkdir -p runtime/central-server
cp central-server-rpi/config/server.ini.example runtime/central-server/server.ini
chmod 600 runtime/central-server/server.ini
```

개발용 주요 항목:

```ini
[mqtt]
host=127.0.0.1
port=1883
client_id=central-server
username=central-server
password=replace-me

[database]
path=runtime/central-server/logistics.db
migration_dir=central-server-rpi/db/migrations

[http]
enabled=true
port=8080
bearer_token=replace-with-a-long-random-token
upload_root=runtime/central-server/uploads

[routing]
qt_client_id=control-center
```

공정 오케스트레이터는 모든 필요한 노드 계약이 준비된 뒤 활성화합니다.

```ini
[process]
enabled=true
input_device_id=PI-INPUT-01
vision_device_id=PI-VISION-01
gripper_device_id=PI-GRIPPER-01
sorting_device_id=PI-SORTING-01
line_tracer_device_id=PI-LT-01
default_destination=3

[sensor_detection]
enabled=true
enter_threshold_cm=10
exit_threshold_cm=12
debounce_count=3
```

현재 Gripper 전용 Raspberry Pi 실행 파일이 없으므로 전체 실제 공정이 준비되기 전에는 `enabled=false`가 안전합니다.

`[sensor_detection]`은 초음파 상자 존재 판정 임계값입니다. STM32는 거리(cm)와 측정
건전성(`OK`/`FAULT`)만 보고하고 판정은 서버가 하므로, **임계값을 바꿀 때 펌웨어를 다시
구울 필요가 없습니다** — 값을 고치고 서버만 재시작하면 4개 센서에 즉시 반영됩니다.
`exit_threshold_cm`은 `enter_threshold_cm` 이상이어야 하고, 두 값의 간격이 히스테리시스
폭입니다(같게 두면 상자가 문턱에 걸쳐 있을 때 판정이 깜빡입니다). `debounce_count`를
늘리면 오탐은 줄지만 반응이 그만큼 늦어집니다(센서 재방문 주기 240ms × 횟수).

## Vision 노드

기준 파일: `device-rpi/config/node.ini.example`

```sh
mkdir -p runtime/vision-node
cp device-rpi/config/node.ini.example runtime/vision-node/vision-node.ini
chmod 600 runtime/vision-node/vision-node.ini
```

서로 다른 기기에서 실행하는 예:

```ini
[device]
device_id=PI-VISION-01
node_name=vision-node-01
ip_address=192.168.0.21

[mqtt]
host=192.168.0.10
port=1883
client_id=PI-VISION-01
username=PI-VISION-01
password=replace-me

[image_upload]
enabled=true
endpoint_url=http://192.168.0.10:8080/api/v1/uploads/images
bearer_token=중앙서버-http-bearer-token과-동일한-값
allow_insecure_http=true
```

운영에서는 HTTP도 HTTPS로 전환하고 `allow_insecure_http=false`, `ca_certificate`를 설정합니다.

## Control Center

```powershell
Copy-Item control-center\config\control-centor.ini.example `
  control-center\config\control-centor.ini
```

```ini
[mqtt]
host=192.168.0.10
port=1883
client_id=control-center
username=control-center
password=replace-me

[http]
image_base_url=http://192.168.0.10:8080/
bearer_token=중앙서버-http-bearer-token과-동일한-값

[logs]
max_buffer_entries=500
```

`http/bearer_token`을 설정하면 운영 로그가 중앙 서버의 과거 이력을 cursor 기반으로 100건씩 조회합니다. 메인
화면은 최신 100건으로 시작하고, 표의 맨 아래에서 계속 아래로 스크롤하면 과거 로그를 100건씩 이어 붙입니다.
화면 표시 모델과 실시간 로그 상태는 모두 `logs/max_buffer_entries`의 한도를 사용합니다. 한도에 도달한 뒤 과거
이력을 더 불러오면 화면 위쪽의 최신 행을 제거해 메모리 사용량을 유지합니다. 토큰을 비워 두면 실시간 MQTT
로그만 표시합니다. 이 설정은 100~500 범위에서 지정할 수 있고 기본값은 500입니다.
`dashboard/*_device_id` 값은 각 노드가 MQTT `sourceId`로 보내는 ID와 정확히 같아야 합니다.

## 환경 변수로 설정 경로 지정

```sh
export LOGISTICS_CENTRAL_SERVER_CONFIG="$PWD/runtime/central-server/server.ini"
export LOGISTICS_DEVICE_CONFIG="$PWD/runtime/input-node/input-node.ini"
```

Control Center:

```powershell
$env:LOGISTICS_CONTROL_CENTER_CONFIG = "C:\path\to\control-centor.ini"
```

명령행 `--config`를 지원하는 중앙서버와 Vision은 환경 변수보다 명시적인 경로를 사용하는 편이 운영 로그 확인에
유리합니다.

## 비밀값 관리

- `.ini.example`에는 실제 비밀번호나 토큰을 넣지 않습니다.
- 실제 INI 권한은 Linux에서 `0600`으로 제한합니다.
- HTTP bearer token과 MQTT 사용자 비밀번호는 서로 다른 값을 사용합니다.
- 인증서 개인키는 저장소나 `runtime/`이 아니라 `/etc/logistics/tls` 같은 운영 전용 경로에 둡니다.
