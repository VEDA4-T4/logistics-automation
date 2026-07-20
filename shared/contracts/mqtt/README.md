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

## Payload와 전송 정책

공통 JSON envelope의 필수 필드는 `protocolVersion`, `messageId`, `messageType`, `sourceId`,
`timestamp`, `data`입니다. 현재 protocol version은 `1.0`이며 timestamp는 timezone을 포함한 ISO 8601
형식을 사용합니다.

제어 명령의 `data`에는 `requestId`, `targetDeviceId`, 선택적인 `componentId`를 포함합니다. 명령을
재전송할 때는 동일한 `messageId`와 `requestId`를 유지합니다.

`BOX_DETECTED`를 저장하면 중앙 서버는 UUID형 `workId`를 발급해 `WORK_CREATED`로 응답합니다. 이후의
위치, 이미지, 바코드, 상품 정보, 목적지 및 `WORK_COMPLETED` payload는 같은 `workId`를 포함해야 합니다.

### Qt 현재 상품 화면 payload

중앙 서버는 현재 상품 변경을 `qt/{clientId}/event`로 전달합니다. `WORK_CREATED`가 새 `workId`로
도착하면 Qt는 이전 상품 정보를 즉시 비우며, 이후 동일한 `workId`의 메시지만 현재 화면에 합칩니다.

- `BARCODE_DETECTED`: `workId`, `recognitionStatus`, 선택적인 `barcode`, `confidence`, `message`
- `PRODUCT_INFO`: `workId`, `recognitionStatus`, 선택적인 `barcode`, `productId`, `productName`, `destination`, `image`
- `PRODUCT_IMAGE`: `workId`, `imageId`, `imageUrl` 또는 `imagePath`, `checksum`, `uploadStatus`
- `DESTINATION_SET`: `workId`, `destination`
- `WORK_COMPLETED`: `workId`, `result`, 선택적인 `message`

`recognitionStatus`는 `SUCCESS`, `FAILED`, `MISSING_DATA` 중 하나를 사용합니다. `image` object에는
`imageId`, `url` 또는 `path`, `checksum`, `uploadStatus`를 사용할 수 있습니다. 이미지 경로는 HTTP(S)
절대 URL 또는 Qt 설정의 `http/image_base_url`을 기준으로 하는 상대 경로이며 이미지 바이너리는 MQTT에
포함하지 않습니다.

`BOX_DETECTED`를 저장하면 중앙 서버는 UUID형 `workId`를 발급해 `WORK_CREATED`로 응답합니다. 이후의 위치,
이미지, 바코드, 상품 정보, 목적지 및 `WORK_COMPLETED` payload는 같은 `workId`를 포함해야 합니다.

- heartbeat: QoS 0, retain 미사용, 5초 간격
- 장치 상태: QoS 0 또는 1, 최신 상태 retain 가능
- 제어·목적지·응답: QoS 1, retain 미사용
- 오류: QoS 1, retain 가능
- 기본 MQTT 응답 제한 3초, 최대 재시도 3회
- heartbeat 10초 미수신 시 `DELAYED`, 15초 미수신 시 `OFFLINE`
