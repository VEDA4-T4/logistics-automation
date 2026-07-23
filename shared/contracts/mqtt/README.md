# MQTT contracts

공통 버전 형식, 호환성 분류와 폐기 절차는 [통신 계약 버전 및 변경 관리](../VERSIONING.md)를 따릅니다.

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
- `PRODUCT_IMAGE`: `workId`, `imageId`, `imagePath`, `checksum`, `uploadStatus`
- `DESTINATION_SET`: `workId`, `destination`
- `WORK_COMPLETED`: `workId`, `result`, 선택적인 `message`

`recognitionStatus`는 `SUCCESS`, `FAILED`, `MISSING_DATA` 중 하나를 사용합니다. `image` object에는
`imageId`, `url` 또는 `path`, `checksum`, `uploadStatus`를 사용할 수 있습니다. `PRODUCT_IMAGE`의
`imagePath`는 HTTP 업로드 응답의 `/uploads/images/...` 경로를 그대로 사용합니다. 이미지 경로는 HTTP(S)
절대 URL 또는 Qt 설정의 `http/image_base_url`을 기준으로 하는 상대 경로이며 이미지 바이너리는 MQTT에
포함하지 않습니다.

### Qt 운영 대시보드 상태

Qt는 중앙 서버가 전달한 메시지를 조합해 전체 공정과 노드별 최신 상태를 표시합니다. 투입 컨베이어, 비전
처리, 그리퍼 이송, 분류 컨베이어, 라인트레이서는 INI의 `dashboard/*_device_id`와 envelope의 `sourceId`를
연결하여 구분합니다. 비전 노드는 상품 윗면의 바코드를 인식하며 상품을 회전해 여러 면을 탐색하지 않습니다.
그리퍼 노드는 비전 노드와 별개이며 상품을 컨베이어 사이로 옮기는 역할만 담당합니다. 그리퍼 노드가 배포되기
전에는 그리퍼 카드가 상태 수신 대기로 유지됩니다. 각 카드는 `status`, `currentState`, `jobId`, `errorCode`와
envelope `timestamp`를 표시합니다.

그리퍼의 기본 상태 흐름은 `PICKING`, `TRANSFERRING`, `PLACING`이며 각각 파지, 컨베이어 사이 이송, 내려놓기를
의미합니다. 상품 회전, 방향 보정 및 바코드 탐색 상태는 그리퍼 프로토콜에 포함하지 않습니다.

- `DEVICE_STATUS`, `HEARTBEAT`: 장치 연결 상태와 현재 상태를 갱신
- `ERROR_OCCURRED`: 해당 장치를 오류로 구분하고 `ERROR` 또는 `CRITICAL`이면 전체 공정도 오류로 표시
- `WORK_CREATED`부터 `WORK_COMPLETED`까지의 작업 이벤트: 공정별 `workId`와 현재 단계를 독립적으로 갱신
- `COMMAND_RESPONSE`, `EMERGENCY_STOP`: 시작·정지·복구·비상정지 결과를 전체 공정 상태에 반영

서로 다른 공정은 각각 다른 `workId`를 동시에 표시할 수 있습니다. 동일한 `messageId`, 공정별 이전
`timestamp`, 해당 공정에서 이미 종료된 `workId`의 지연 메시지는 화면 상태를 되돌리지 않도록 무시합니다.
중앙 서버는 장치 heartbeat의 `status`, `currentState`, `jobId`, `errorCode`를 기존 `DEVICE_STATUS`로 변환해
Qt 상태 토픽에 전달하므로 별도의 메시지 타입이나 토픽을 추가하지 않습니다.

`BOX_DETECTED`를 저장하면 중앙 서버는 UUID형 `workId`를 발급해 `WORK_CREATED`로 응답합니다. 이후의 위치,
이미지, 바코드, 상품 정보, 목적지 및 `WORK_COMPLETED` payload는 같은 `workId`를 포함해야 합니다.

- heartbeat: QoS 0, retain 미사용, 5초 간격
- 장치 상태: QoS 0 또는 1, 최신 상태 retain 가능
- 제어·목적지·응답: QoS 1, retain 미사용
- 오류: QoS 1, retain 가능
- 기본 MQTT 응답 제한 3초, 최대 재시도 3회
- heartbeat 10초 미수신 시 `DELAYED`, 15초 미수신 시 `OFFLINE`
