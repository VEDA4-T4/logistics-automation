# MQTT contracts

MQTT topic, payload, QoS, retain, Last Will 규칙을 중앙에서 관리하는 위치입니다. 구현 기준은
`설계서.pdf`의 MQTT Topic/Payload 설계(83~100쪽)입니다.

- 명령/응답은 동일한 `requestId`로 연계합니다.
- 장치 상태는 heartbeat와 Last Will을 함께 사용합니다.
- 중앙관제와 장치 노드는 중앙 서버의 broker를 통해서만 통신합니다.
- JSON Schema를 추가할 때는 메시지 종류별 파일로 분리합니다.

## Topic

- Qt 요청: `server/request/{clientId}`
- Qt 수신: `qt/{clientId}/{response|status|event|error}`
- 장치 통신: `device/{deviceId}/{register|command|response|status|event|error|heartbeat}`
- 전체 명령: `system/broadcast/command`
- 서버 상태: `server/status`, `server/heartbeat`

Topic 생성·분석 함수와 중앙 서버용 wildcard 구독 상수는
`logistics/contracts/mqtt_topic.hpp`에 있습니다. ID는 빈 문자열이 아니며 `/`, `+`, `#`을 포함할 수
없습니다.

## Payload와 전송 정책

공통 JSON envelope의 필수 필드는 `protocolVersion`, `messageId`, `messageType`, `sourceId`,
`timestamp`, `data`입니다. 현재 protocol version은 `1.0`이며 timestamp는 UTC의 `Z` 또는 명시적인
offset을 포함한 ISO 8601 형식을 사용합니다.

`sourceId`는 메시지를 발행한 Qt, 중앙 서버 또는 장치 ID입니다. 제어 명령의 `data`에는
`requestId`, `targetDeviceId`, 선택적인 `componentId`를 포함합니다. `targetDeviceId`는 명령을 받는
Raspberry Pi/STM32 장치를 식별하며, `componentId`는 그 장치가 제어하는 개별 모터나 센서를
식별합니다. 명령을 재전송할 때는 동일한 `messageId`와 `requestId`를 유지합니다.

`BOX_DETECTED`를 저장하면 중앙 서버는 UUID형 `workId`를 발급해 `WORK_CREATED`로 응답합니다. 이후의 위치,
이미지, 바코드, 상품 정보, 목적지 및 `WORK_COMPLETED` payload는 같은 `workId`를 포함해야 합니다.

- heartbeat: QoS 0, retain 미사용, 5초 간격
- 장치 상태: QoS 0 또는 1, 최신 상태 retain 가능
- 제어·목적지·응답: QoS 1, retain 미사용
- 오류: QoS 1, retain 가능
- 기본 MQTT 응답 제한 3초, 최대 재시도 3회
- heartbeat 10초 미수신 시 `DELAYED`, 15초 미수신 시 `OFFLINE`

메시지·명령·응답·연결 상태 enum과 전송 정책은 `logistics/contracts/mqtt_message.hpp`에 있습니다.
