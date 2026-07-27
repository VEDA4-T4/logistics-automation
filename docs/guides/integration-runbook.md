# 통합 실행 가이드

이 문서는 Mosquitto, 중앙서버, Vision, Control Center를 서로 다른 기기에서 실행하는 기본 통합 순서입니다.

## 1. 준비 상태 확인

예시 네트워크:

| 구성 요소 | 주소 |
| --- | --- |
| Mosquitto + Central Server | `192.168.0.10` |
| Vision Raspberry Pi | `192.168.0.21` |
| Control Center PC | `192.168.0.30` |

개발 단계 포트:

- MQTT: TCP `1883`
- HTTP 이미지 업로드/다운로드: TCP `8080`
- RTSP: 카메라 설정에 따른 포트, 일반적으로 TCP `554`

운영 TLS 전환 후 MQTT는 `8883`, HTTP는 별도의 HTTPS 포트를 사용합니다. 현재 MQTT 클라이언트 TLS 지원 상태는
[Mosquitto 보안 및 TLS](../../deploy/mosquitto/README.md)를 먼저 확인하세요.

## 2. Mosquitto 시작

```sh
sudo systemctl enable --now mosquitto
sudo systemctl status mosquitto --no-pager
ss -lnt | grep ':1883'
```

인증과 ACL은 [Mosquitto 보안 가이드](../../deploy/mosquitto/README.md)에 따라 설정합니다.

## 3. 중앙서버 빌드와 설정

```sh
export LOGISTICS_UPLOAD_TOKEN='충분히-긴-임의의-토큰'
export LOGISTICS_MQTT_HOST='127.0.0.1'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-central-server.sh
```

실행:

```sh
./build-central/central-server-rpi/logistics_central_server \
  --config runtime/central-server/server.ini
```

정상 로그 예:

```text
[server][INFO] central server started
```

`registered devices=0`은 오류가 아닙니다. 아직 장치 노드가 등록되지 않았다는 뜻입니다.

## 4. Vision 노드 빌드와 설정

Vision Raspberry Pi에서 실행합니다.

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_MQTT_HOST='192.168.0.10'
export LOGISTICS_UPLOAD_TOKEN='중앙서버와-동일한-토큰'
export LOGISTICS_DEVICE_ID='PI-VISION-01'
export LOGISTICS_DEVICE_IP='192.168.0.21'
export LOGISTICS_INSTALL_DEPENDENCIES=1
./deploy/scripts/setup-vision-node.sh
```

SSH나 systemd 환경에서는 headless로 실행합니다.

```sh
./build-vision/device-rpi/logistics_vision_node \
  --headless \
  --config runtime/vision-node/vision-node.ini
```

`--camera` 옵션은 없습니다. 카메라 인덱스는 현재 기본값을 사용하며 지원 옵션은 `--width`, `--height`, `--fps`,
`--headless`, `--config`입니다.

## 5. Control Center 실행

`control-center/config/control-centor.ini`에서 다음 주소를 확인합니다.

```ini
[mqtt]
host=192.168.0.10
port=1883

[http]
image_base_url=http://192.168.0.10:8080/
```

Qt Creator에서 `logistics_control_center`를 실행합니다. MQTT가 연결되면 장치 등록·상태·작업 이벤트가 대시보드에
표시됩니다.

## 6. 연결 확인

Vision Pi에서:

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
./deploy/scripts/check-connectivity.sh
```

Windows Control Center PC에서:

```powershell
Test-NetConnection 192.168.0.10 -Port 1883
Test-NetConnection 192.168.0.10 -Port 8080
```

TCP 성공은 포트 접근만 확인합니다. MQTT 인증·ACL이나 HTTP 요청 처리 성공까지 보장하지는 않습니다.

## 7. Vision 작업 시나리오

1. Vision 노드가 MQTT에 연결하고 등록·heartbeat를 발행합니다.
2. 상품이 안정적으로 감지되면 `BOX_DETECTED`를 발행합니다.
3. 중앙서버가 UUID `workId`를 만들고 `WORK_CREATED`를 Vision과 Control Center에 보냅니다.
4. Vision이 `POSITION_DETECTED`, `BARCODE_DETECTED`를 발행합니다.
5. 중앙서버가 상품 DB를 조회해 `PRODUCT_INFO`를 생성합니다.
6. 인식 성공 이미지가 HTTP로 업로드되고 `PRODUCT_IMAGE`가 발행됩니다.
7. Control Center는 다음 성공 이미지가 도착할 때까지 마지막 성공 이미지를 유지합니다.

기본 테스트 바코드는 상품 DB에 존재하는 `5901234123457`을 사용할 수 있습니다.

## 8. 전체 공정 활성화 전 확인

중앙 상태 머신은 다음 순서를 모델링합니다.

```text
Input → Vision → Gripper → Sorting → Line Tracer → Completed
```

다만 현재 `main`에는 Gripper 전용 Raspberry Pi 실행 파일이 없고 Input/Sorting/Line Tracer는 공통 런타임
기반입니다. 모든 장치별 MQTT↔UART 구현과 상태 보고가 합쳐지기 전에는 중앙서버 `[process] enabled=false`를
유지합니다. 준비되지 않은 상태에서 활성화하면 다음 장치 명령 전송 시 공정이 `ERROR`로 전환됩니다.

## 9. 종료 순서

1. Control Center에서 공정을 정지합니다.
2. Device 노드를 종료합니다.
3. 중앙서버를 종료합니다.
4. 유지보수가 필요한 경우에만 Mosquitto를 종료합니다.

systemd 자동 시작은 [systemd 운영 가이드](../../deploy/systemd/README.md)를 참고하세요.
