# UART contracts

장치 Raspberry Pi와 STM32 사이의 패킷 정의를 관리하는 위치입니다.

패킷은 설계서의 `START | LENGTH | COMMAND | DATA | CHECKSUM | END` 형식을 기준으로 하며,
명령 ID, 결과 코드, 제한 시간과 재전송 정책을 이 디렉터리에서 단일 소스로 관리합니다.

