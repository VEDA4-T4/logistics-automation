# UART contracts

공통 버전 형식, 호환성 분류와 폐기 절차는 [통신 계약 버전 및 변경 관리](../VERSIONING.md)를 따릅니다.

장치 Raspberry Pi와 STM32 사이의 패킷 정의를 관리하는 위치입니다.

패킷은 다음 frame을 사용합니다.

```text
SOF | VERSION | SEQUENCE | COMMAND | LENGTH | PAYLOAD | CRC16
```

- `SOF`: frame 시작을 나타내는 고정 byte
- `VERSION`: UART protocol version
- `SEQUENCE`: 명령과 응답을 연결하는 1-byte 순번
- `COMMAND`: 장치 공통 또는 장치별 명령 ID
- `LENGTH`: payload byte 길이
- `PAYLOAD`: 명령 인자, 센서 값 또는 처리 결과
- `CRC16`: `VERSION`부터 `PAYLOAD`까지 계산한 CRC16-CCITT

다중 byte 정수는 little-endian이며 고정 크기 정수형을 사용합니다. 응답은 `ACK`, `NACK`, `BUSY`,
`SUCCESS`, `ERROR`를 구분하고, 최대 payload 크기와 timeout/재시도 횟수는 codec 구현 시 공통
상수로 정의합니다. 비상정지는 일반 명령 queue와 무관하게 최우선으로 처리하며 자동 재시작하지
않습니다.
