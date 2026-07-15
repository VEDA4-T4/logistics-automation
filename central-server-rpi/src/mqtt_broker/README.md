# MQTT broker connection

Mosquitto broker는 애플리케이션에 내장하지 않고 별도 서비스로 실행합니다. 이 디렉터리의 서버 MQTT 모듈은
`libmosquitto`를 사용하여 다음 기능을 제공합니다.

- `server.ini`의 `[mqtt]` 접속 정보 로딩 및 검증
- 비동기 broker 연결, 발행, 구독
- 지수 backoff 기반 자동 재연결
- 연결 또는 재연결 성공 시 서버 필수 topic 재구독
- 연결 거부, 연결 단절, 발행 및 구독 실패 로그

기본 구독은 `shared/include/logistics/contracts/mqtt_topic.hpp`에 정의된 Qt 요청 및 장치의 등록, 응답, 상태,
이벤트, 오류, heartbeat wildcard topic입니다. 실제 broker 계정, ACL, TLS 설정은 `deploy/mosquitto/`에서
관리합니다.

Ubuntu/Raspberry Pi에서는 빌드 전에 개발 패키지를 설치합니다.

```sh
sudo apt-get update
sudo apt-get install -y libmosquitto-dev
```

실행 파일의 첫 번째 인수 또는 `LOGISTICS_CENTRAL_SERVER_CONFIG` 환경 변수로 설정 파일 경로를 지정합니다.
둘 다 없으면 `config/server.ini`를 사용합니다. 로그는 표준 출력과 표준 오류로 기록되므로 systemd 환경에서는
journal에서 확인할 수 있습니다.

```sh
./build/central-server-rpi/logistics_central_server central-server-rpi/config/server.ini
```
