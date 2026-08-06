# 통신 계약 안내

구성 요소 간 통신을 변경할 때 구현보다 계약을 먼저 수정합니다.

| 구간 | 프로토콜 | 기준 문서 |
| --- | --- | --- |
| Control Center ↔ Central Server ↔ Device Node | MQTT | [MQTT 계약](../../shared/contracts/mqtt/README.md) |
| Vision/Device Node ↔ Central Server | HTTP(S) | [HTTP 업로드 계약](../../shared/contracts/http/README.md) |
| Device Raspberry Pi ↔ STM32 | UART | [UART 계약](../../shared/contracts/uart/README.md) |

계약 버전과 호환성 규칙은 [VERSIONING](../../shared/contracts/VERSIONING.md)을 따릅니다.

## MQTT 기본 흐름

```text
BOX_DETECTED
  → WORK_CREATED
  → POSITION_DETECTED
  → BARCODE_DETECTED
  → PRODUCT_INFO
  → Gripper command/status
  → Sorting DESTINATION_SET/status
  → Line Tracer DESTINATION_SET/status
  → WORK_COMPLETED
```

모든 작업 이벤트는 중앙서버가 발급한 동일한 UUID형 `workId`를 사용합니다. `messageId`는 QoS 1 재전송의 중복
판단에 사용되므로 새 이벤트마다 고유해야 합니다.

## 변경 체크리스트

1. `shared/include/logistics/contracts`의 타입과 검증 규칙을 변경합니다.
2. 직렬화·역직렬화 및 topic/message 조합 테스트를 추가합니다.
3. 중앙서버 저장 스키마가 새 상태 값을 허용하는지 확인합니다.
4. Control Center, Device Node, STM32 중 영향을 받는 소비자를 모두 갱신합니다.
5. 기존 메시지와의 호환 또는 버전 상승 여부를 문서화합니다.
