# Line-tracer UART test tool

Raspberry Pi에서 STM32 line-tracer controller의 binary UART protocol을 직접 시험하는 대화형 C++ 도구다.
저장소의 `logistics::uart` codec/parser를 그대로 사용하므로 펌웨어와 동일한 CRC 및 frame 규격을 사용한다.

## Build

운영 device node와 OpenCV를 빌드하지 않고 이 도구만 구성할 수 있다.

```sh
cmake -S . -B build-uart-test \
  -DBUILD_TESTING=OFF \
  -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
  -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
  -DLOGISTICS_BUILD_DEVICE_NODES=OFF \
  -DLOGISTICS_BUILD_LINETRACER_UART_TEST=ON
cmake --build build-uart-test --target logistics_linetracer_uart_test -j4
```

UART 장치를 열지 않고 공용 codec/parser와 주요 VEDA-128 명령 frame을 먼저 확인할 수 있다.

```sh
./build-uart-test/logistics_linetracer_uart_test --self-test
```

## Run

GPIO UART를 `/dev/serial0`으로 사용하는 경우:

```sh
./build-uart-test/logistics_linetracer_uart_test /dev/serial0
```

USB-UART 또는 `vedauart` driver를 사용한다면 각각 `/dev/ttyUSB0`, `/dev/vedauart`처럼 실제 장치 경로를
인자로 전달한다. UART는 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control로 설정된다.

프로그램을 시작하면 STM32의 heartbeat와 비동기 event를 계속 출력하며 다음 명령을 받을 수 있다.

```text
ping
status
position 1
route 4660 2
stop 4660
reset
resume
estop
keepalive on
keepalive off
quit
```

`route`의 job과 route는 각각 `1..65535`, `1..3` 범위다. 16진수 job ID도 사용할 수 있다.

```text
route 0x1234 2
stop 0x1234
```

작업이 활성화되면 STM32의 RX link timeout을 피하기 위해 먼저 `keepalive on`을 실행한다. 도구는 매초
PING을 전송하고 STM32 응답은 다른 명령과 함께 출력한다.

## VEDA-128 manual test sequence

모터를 안전하게 띄우거나 모터 전원을 분리하고, E-Stop·line·ultrasonic·load safety input이 정상인 상태에서
다음 순서로 시험한다.

```text
keepalive on
position 1
route 0x1234 2
status
stop 0x1234
status
resume
status
```

- 정상 STOP: ACK, STOPPED state, 같은 job/route 유지
- 안전 조건 미충족 RESUME: NACK, STOPPED 유지
- line CENTERED가 200ms 이상 안정된 뒤 RESUME: ACK와 STATE_CHANGED event
- `estop`: 즉시 FAULT event와 EMERGENCY_STOP state. 이 명령은 별도 ACK 대신 비동기 fault로 보고된다.
- 안전 입력이 해제된 뒤 `reset`, `resume`: 각각 ACK/NACK 결과와 상태 변경을 확인한다.

ACK/NACK frame에는 상세 error가 없으므로 NACK 뒤에 `status`를 실행해 SENSOR, BUSY,
EMERGENCY_STOP 등의 error를 확인한다.
