# 프로젝트 구조

## Project Structure

```text
logistics-automation/
├── CMakeLists.txt
├── cmake/                         # 공통 CMake 정책
├── control-center/                # Qt 중앙관제 (PC)
│   ├── include/
│   ├── src/
│   ├── ui/                        # Qt Designer 화면
│   ├── resources/                 # 아이콘, 번역, 스타일
│   └── config/
├── central-server-rpi/            # 중앙 서버 Raspberry Pi
│   ├── include/
│   ├── src/
│   │   ├── mqtt_broker/           # 외부 broker 관리/상태 어댑터
│   │   ├── mqtt_handler/          # 메시지 수신, 검증, 라우팅
│   │   ├── device_manager/        # 장치 등록/상태/명령 라우팅
│   │   ├── db_manager/            # SQLite repository/transaction
│   │   └── log_manager/           # 작업/오류/보안 로그
│   ├── config/
│   └── db/migrations/
├── device-rpi/                    # 공정별 장치 Raspberry Pi
│   ├── common/
│   │   ├── mqtt_client/
│   │   ├── uart_bridge/
│   │   ├── device_status/
│   │   └── config/
│   ├── input-node/                # 투입/컨베이어
│   ├── camera-node/               # 상자 검출/좌표 계산
│   ├── recognition-node/          # 바코드 인식/이미지 저장
│   ├── sorting-node/              # 분류 장치 중계
│   ├── linetracer-node/           # 운반 상태 중계
│   └── logs/
├── stm32/
│   ├── input-controller/
│   ├── rotation-controller/
│   ├── sorting-controller/
│   ├── linetracer-controller/
│   └── common/                    # UART codec/안전/공통 드라이버
├── shared/
│   ├── include/                   # 공통 C++ 도메인 타입
│   ├── contracts/mqtt/            # Topic/Payload schema
│   ├── contracts/uart/            # Packet/Command/Result 정의
│   └── tests/
├── deploy/
│   ├── mosquitto/
│   └── systemd/
└── docs/
    └── architecture/
```

## Dependency Rules

1. `control-center`와 `device-rpi`는 중앙 서버 DB에 직접 접근하지 않는다.
2. PC/Raspberry Pi 간 통신은 MQTT, 장치 Raspberry Pi/STM32 간 통신은 UART만 사용한다.
3. MQTT topic/payload 및 UART command/result 변경은 먼저 `shared/contracts`에 반영한다.
4. 장치별 하드웨어 제어는 `stm32`에 두고, `device-rpi`에는 GPIO/PWM 제어 로직을 넣지 않는다.
5. 비상정지는 STM32가 네트워크와 무관하게 즉시 처리하며 자동 재시작하지 않는다.
6. 실행 환경별 값은 소스에 넣지 않고 `.ini.example`을 복사한 로컬 설정 또는 배포 설정으로 주입한다.

## Build Targets

- 기본 호스트 CMake: `shared`, `central-server-rpi`, `device-rpi`
- 선택 빌드: `control-center` (`LOGISTICS_BUILD_CONTROL_CENTER=ON`, Qt 6 필요)
- 별도 펌웨어 빌드: `stm32/*-controller` (STM32CubeIDE/ARM toolchain)

