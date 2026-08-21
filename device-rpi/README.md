# Device Raspberry Pi Nodes

공정별 Raspberry Pi에서 MQTT와 STM32 UART 사이를 연결하고 registration, heartbeat, 상태, 이벤트와 오류를
중앙서버에 보고합니다.

## 실행 타깃

| 타깃 | 현재 `main` 상태 |
| --- | --- |
| `logistics_input_node` | 공통 `NodeRuntime` |
| `logistics_vision_node` | 카메라·바코드·이미지 업로드 구현 |
| `logistics_sorting_node` | 공통 `NodeRuntime` |
| `logistics_linetracer_node` | 공통 `NodeRuntime` |
| Gripper node | 전용 Raspberry Pi 타깃 미구현 |

공통 런타임 타깃이 존재한다는 사실과 장치별 MQTT↔UART 공정 동작이 완성되었다는 의미는 다릅니다.

## 설치와 빌드

- [Ubuntu/Raspberry Pi 설치](../docs/setup/ubuntu-rpi.md)
- [빌드 및 테스트](../docs/guides/compilation-and-tests.md)
- [VEDAUART 드라이버](kernel/vedauart/README.md)

Vision 자동 설정:

```sh
export LOGISTICS_CENTRAL_HOST='192.168.0.10'
export LOGISTICS_UPLOAD_TOKEN='중앙서버와-동일한-토큰'
export LOGISTICS_DEVICE_ID='PI-VISION-01'
./deploy/scripts/setup-vision-node.sh
```

## 설정

기준 파일은 `config/node.ini.example`입니다. 각 장치는 고유한 `device_id`, `client_id`, MQTT 사용자와 로컬 INI를
사용해야 합니다. 자세한 내용은 [런타임 설정](../docs/guides/runtime-configuration.md)을 참고하세요.

## 실행

Vision:

```sh
./build-vision/device-rpi/logistics_vision_node \
  --headless \
  --config runtime/vision-node/vision-node.ini
```

공통 NodeRuntime 기반 실행 파일은 INI 경로를 위치 인자로 받습니다.

```sh
./build-device/device-rpi/logistics_input_node runtime/input-node/input-node.ini
./build-device/device-rpi/logistics_sorting_node runtime/sorting-node/sorting-node.ini
./build-device/device-rpi/logistics_linetracer_node runtime/linetracer-node/linetracer-node.ini
```

`LOGISTICS_DEVICE_CONFIG` 환경 변수로 공통 노드 설정 경로를 지정할 수도 있습니다.

## 관련 문서

- [Vision 동작](vision-node/README.md)
- [공통 MQTT client](common/mqtt_client/README.md)
- [UART bridge](common/uart_bridge/README.md)
- [장치 상태](common/device_status/README.md)
- [통신 계약](../docs/guides/communication-contracts.md)
- [문제 해결](../docs/guides/operations-troubleshooting.md)
