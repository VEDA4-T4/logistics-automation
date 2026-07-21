# STM32F401RE 통합 컨베이어 컨트롤러 FreeRTOS 설정 매뉴얼

## 1. 문서 목적

이 문서는 `NUCLEO-F401RE` 보드에서 두 대의 Raspberry Pi와 UART로 통신하고, 두 개의 컨베이어와 분류 게이트를 제어하기 위한 STM32CubeMX 및 FreeRTOS 초기 설정을 정리한다.

현재 구성하는 펌웨어의 주요 담당 태스크는 다음과 같다.

- `CommRxTask`: USART1·USART6 데이터 수신 및 명령 분배
- `InputControlTask`: 투입 컨베이어(컨베이어 1) 제어
- `SortingControlTask`: 분류 컨베이어(컨베이어 2)와 분류 게이트 제어

> 이 문서는 VEDA-168 통신 수신 태스크 작업을 시작하면서 확정한 통합 컨베이어 컨트롤러 설정을 기준으로 작성되었다.

---

## 2. 개발 환경

| 구분 | 설정 |
|---|---|
| MCU 보드 | NUCLEO-F401RE |
| MCU | STM32F401RETx |
| 설정 도구 | STM32CubeMX |
| 개발 IDE | STM32CubeIDE |
| HAL 패키지 | STM32Cube FW_F4 V1.28.3 |
| RTOS | FreeRTOS, CMSIS-RTOS V2 |
| 프로젝트 이름 | `integrated_conveyor_controller` |
| Git 브랜치 | `feature/conveyor-controller/comm-rx-task` |

권장 프로젝트 위치는 다음과 같다.

```text
C:\Users\3-20\OneDrive\Desktop\logistics-automation\stm32\conveyor-controller
```

---

## 3. 시스템 구성

```text
투입 Raspberry Pi
        │ USART1
        ▼
┌──────────────────────── STM32F401RE ────────────────────────┐
│ CommRxTask                                                   │
│   ├─ 투입 명령 ───────────────▶ InputControlTask             │
│   │                              └─ 투입 컨베이어(1) 제어     │
│   └─ 분류 장치 제어 명령 ─────▶ SortingControlTask           │
│                                  ├─ 분류 컨베이어(2) 제어    │
│                                  └─ 분류 게이트 제어         │
└──────────────────────────────────────────────────────────────┘
        ▲
        │ USART6
분류 Raspberry Pi
```

`투입 컨베이어`, `분류 컨베이어`, `분류 게이트`는 별도의 FreeRTOS 태스크가 아니다. 각각 `InputControlTask`와 `SortingControlTask`가 제어하는 하드웨어 출력 대상이다.

바코드·상품·목적지 판단은 서버와 Raspberry Pi에서 모두 완료한다. STM32는 처리 결과로 생성된 시작·정지·속도·게이트 위치 같은 최종 장치 제어 명령만 수행한다.

---

## 4. UART 배선

### 4.1 USART1 — 투입 Raspberry Pi

USART1은 NUCLEO 보드의 Arduino 헤더를 사용한다.

| 신호 | STM32 핀 | Arduino 헤더 | Raspberry Pi 연결 |
|---|---|---|---|
| USART1_TX | PA9 | D8 | Raspberry Pi RX |
| USART1_RX | PA10 | D2 | Raspberry Pi TX |
| GND | GND | GND | Raspberry Pi GND |

### 4.2 USART6 — 분류 Raspberry Pi

USART6은 PC6·PC7 대신 PA11·PA12로 리맵하여 사용한다. 이 구성은 PC7을 분류 게이트용 PWM 핀으로 확보할 수 있다는 장점이 있다.

| 신호 | STM32 핀 | Morpho 헤더 | Raspberry Pi 연결 |
|---|---|---|---|
| USART6_TX | PA11 | CN10 14번 | Raspberry Pi RX |
| USART6_RX | PA12 | CN10 12번 | Raspberry Pi TX |
| GND | GND | GND | Raspberry Pi GND |

### 4.3 배선 주의사항

- UART의 TX와 RX는 교차 연결한다.
- STM32와 Raspberry Pi의 GND를 반드시 공통으로 연결한다.
- 두 장치 모두 3.3V UART 로직을 사용한다.
- Raspberry Pi의 5V 전원 핀을 STM32 UART 신호 핀에 연결하지 않는다.
- USART2는 NUCLEO의 ST-LINK 가상 COM 포트와 연결되어 있으므로 본 구성에서는 사용하지 않는다.

---

## 5. 컨베이어 모터 핀 구성

TB6612FNG 모터 드라이버를 기준으로 한 핀 구성이다.

### 5.1 투입 컨베이어 — 컨베이어 1

`InputControlTask`가 제어한다.

| TB6612FNG 신호 | STM32 핀 | Arduino 헤더 | 용도 |
|---|---|---|---|
| PWMA | PA8 | D7 | 컨베이어 1 속도 PWM |
| AIN1 | PB5 | D4 | 컨베이어 1 방향 제어 |
| AIN2 | PB4 | D5 | 컨베이어 1 방향 제어 |
| AO1/AO2 | - | - | 컨베이어 1 모터 출력 |

### 5.2 분류 컨베이어 — 컨베이어 2

`SortingControlTask`가 제어한다.

| TB6612FNG 신호 | STM32 핀 | Arduino 헤더 | 용도 |
|---|---|---|---|
| PWMB | PB9 | D14 | 컨베이어 2 속도 PWM (`TIM11_CH1`, 20 kHz) |
| BIN1 | PA7 | D11 | 컨베이어 2 방향 제어 |
| BIN2 | PA6 | D12 | 컨베이어 2 방향 제어 |
| BO1/BO2 | - | - | 컨베이어 2 모터 출력 |

### 5.3 분류 게이트 MG90S

| MG90S 신호 | STM32 핀 | Arduino 헤더 | 용도 |
|---|---|---|---|
| PWM | PC7 | D9 | 게이트 위치 PWM (`TIM3_CH2`, 50 Hz) |
| VCC | 외부 5 V / 3 A | - | 서보 전용 외부 전원; STM32 GPIO·3.3 V 핀에서 공급하지 않음 |
| GND | 공통 GND | GND | STM32·모터 드라이버·5 V 전원의 GND를 공통 연결 |

### 5.4 공통 활성화 핀

| TB6612FNG 신호 | STM32 핀 | Arduino 헤더 | 용도 |
|---|---|---|---|
| STBY | PB6 | D10 | 두 모터 채널 공통 활성화 |

### 5.5 전원 및 JMOD 점퍼 구성

| 전원 대상 | 공급원 | 연결 |
|---|---|---|
| Nucleo 및 JMOD 로직 | Nucleo ST-LINK USB | Nucleo `5V` 헤더 → JMOD `5V` |
| TT 모터 2개 | 외부 6 V / 3 A | 양극 → JMOD `VIN`, 음극 → JMOD `GND` |
| MG90S | 외부 5 V / 3 A | 양극 → MG90S `VCC`, 음극 → MG90S `GND` |
| 공통 기준 전압 | 모든 전원과 장치 | Nucleo·JMOD·6 V 어댑터·5 V 어댑터·MG90S의 GND 공통 연결 |

- 외부 6 V 모터 전원을 사용할 때 JMOD의 모터 전원 선택 점퍼는 `VCC–VIN` 위치로 설정한다.
- 외부 5 V 어댑터의 양극은 MG90S에만 연결하고 Nucleo의 `5V` 헤더에는 연결하지 않는다.
- JMOD의 `5V`는 로직 전원이며, TT 모터 구동 전원은 `VIN`을 통해 별도로 공급한다.
- USB 및 모든 외부 전원을 끈 상태에서 배선한 뒤 극성과 공통 GND를 확인하고 전원을 인가한다.

### 5.6 JMOD-MOTOR-1 제어 기준

- 모터 드라이버는 TB6612FNG 기반 `JMOD-MOTOR-1`을 사용한다.
- 컨베이어 1의 `PWMA`는 `TIM1_CH1`에서 20 kHz PWM으로 구동한다.
- 컨베이어 2의 `PWMB`는 `TIM11_CH1`에서 20 kHz PWM으로 구동한다.
- MG90S 게이트는 `TIM3_CH2`의 50 Hz PWM으로 구동하며, 실제 펄스 범위는 기구물 조립 후 안전 각도를 기준으로 보정한다.
- MG90S 기본 펄스는 Home 1500 us, 목적지 1/2/3은 각각 1000/1500/2000 us이며 `sorting_gate_mg90s.h`의 설정값으로 보정한다.
- MG90S 명령 후 기본 500 ms 동안 Task를 블로킹하지 않고 정착을 확인한다. `CYCLE_COMPLETE`는 Home 정착이 끝난 뒤에만 송신한다.
- MG90S의 전원은 별도 5 V / 3 A 어댑터에서 공급한다. 어댑터의 5 V 양극은 서보 VCC에만 연결하고, 어댑터 GND는 STM32 및 JMOD-MOTOR-1의 GND와 공통으로 연결한다.
- 컨베이어 정방향은 `AIN1=HIGH`, `AIN2=LOW`로 정의한다. 실제 기구 방향이 반대면 코드 대신 `AO1`과 `AO2` 배선을 서로 바꾼다.
- 분류 컨베이어 정방향은 `BIN1=HIGH`, `BIN2=LOW`로 정의한다. 실제 기구 방향이 반대면 `BO1`과 `BO2` 배선을 서로 바꾼다.
- 정지 시 PWM을 0%로 내리고 `AIN1=LOW`, `AIN2=LOW`로 둔다.
- 부팅·초기화·일반 STOP에서는 공용 `STBY`를 올리지 않는다. 실제 START에서만 활성화를 요청하며, 한 채널의 STOP은 다른 채널을 멈추지 않도록 `STBY`를 내리지 않는다.
- SafetyTask는 비상 정지 시 `conveyor_motor_power_latch_disable()`로 공용 `STBY`를 LOW로 내린 뒤 Input·Sorting ControlTask의 stop notify API를 호출해야 한다. 두 태스크가 모두 안전 출력을 확인한 뒤에만 release notify와 latch 해제를 진행하며 일반 RESET 명령으로 latch를 해제하지 않는다.
- 현재 조달 조건에서는 별도 레벨 시프터 없이 STM32의 3.3 V GPIO를 JMOD-MOTOR-1 제어 입력에 직접 연결한다. JMOD-MOTOR-1이 Raspberry Pi 연결을 지원한다고 안내하지만, TB6612FNG 로직 전원이 5 V일 때 3.3 V HIGH는 데이터시트상 보장 범위보다 낮으므로 실제 모터를 무부하 상태에서 검증해야 한다. 기동 실패·떨림·간헐 정지가 발생하면 펌웨어보다 로직 전압 호환 문제를 먼저 확인한다.
- STM32, JMOD-MOTOR-1, 모터 전원은 GND를 공통으로 연결한다.

---

## 6. STM32CubeMX 프로젝트 생성

### 6.1 보드 선택

1. STM32CubeMX를 실행한다.
2. **New Project**를 선택한다.
3. **Board Selector**에서 `NUCLEO-F401RE`를 검색한다.
4. 해당 보드를 선택하고 프로젝트를 생성한다.

보드가 아닌 MCU를 직접 선택한 경우 MCU가 `STM32F401RETx`인지 확인한다.

### 6.2 RCC 설정

메뉴 경로:

```text
Pinout & Configuration
→ System Core
→ RCC
```

다음과 같이 설정한다.

| 항목 | 값 |
|---|---|
| High Speed Clock (HSE) | BYPASS Clock Source |
| Low Speed Clock (LSE) | Disable |

NUCLEO 보드에서는 ST-LINK가 공급하는 외부 클럭을 사용하므로 HSE를 `BYPASS Clock Source`로 설정한다.

### 6.3 SYS 설정

메뉴 경로:

```text
Pinout & Configuration
→ System Core
→ SYS
```

| 항목 | 값 |
|---|---|
| Debug | Serial Wire |
| Timebase Source | TIM5 |

STM32F401RE에는 TIM6가 없으므로 HAL Timebase로 TIM5를 사용한다. FreeRTOS가 SysTick을 RTOS Tick으로 사용하기 때문에 HAL Timebase는 SysTick과 분리한다.

### 6.4 Clock Configuration

`Clock Configuration` 탭에서 시스템 클럭을 다음과 같이 구성한다.

| 항목 | 값 |
|---|---|
| HSE 입력 | 8 MHz |
| SYSCLK | 84 MHz |
| HCLK | 84 MHz |
| APB1 Peripheral Clock | 42 MHz |
| APB2 Peripheral Clock | 84 MHz |

CubeMX의 **Resolve Clock Issues**를 사용한 경우에도 최종 SYSCLK가 STM32F401RE의 최대 클럭인 84 MHz인지 확인한다.

---

## 7. USART 설정

메뉴 경로:

```text
Pinout & Configuration
→ Connectivity
→ USART1 / USART6
```

USART1과 USART6 모두 `Asynchronous` 모드로 활성화하고 기본 통신 설정을 동일하게 맞춘다.

| 항목 | 값 |
|---|---|
| Mode | Asynchronous |
| Baud Rate | 115200 Bits/s |
| Word Length | 8 Bits |
| Parity | None |
| Stop Bits | 1 |
| Hardware Flow Control | None |
| Oversampling | 16 |

Pinout 화면에서 다음 핀 매핑을 반드시 확인한다.

| 주변장치 | TX | RX |
|---|---|---|
| USART1 | PA9 | PA10 |
| USART6 | PA11 | PA12 |

---

## 8. DMA 설정

각 USART의 **DMA Settings** 탭에서 RX와 TX DMA를 추가한다.

| 용도 | DMA 매핑 | Channel | Mode | Priority |
|---|---|---:|---|---|
| USART1_RX | DMA2 Stream 5 | Channel 4 | Circular | High |
| USART1_TX | DMA2 Stream 7 | Channel 4 | Normal | Medium |
| USART6_RX | DMA2 Stream 1 | Channel 5 | Circular | High |
| USART6_TX | DMA2 Stream 6 | Channel 5 | Normal | Medium |

공통 DMA 설정은 다음과 같다.

| 항목 | 값 |
|---|---|
| Peripheral Increment Address | Disable |
| Memory Increment Address | Enable |
| Peripheral Data Width | Byte |
| Memory Data Width | Byte |

RX는 계속 데이터를 받아야 하므로 `Circular` 모드를 사용한다. TX는 전송 요청마다 DMA를 시작하므로 `Normal` 모드를 사용한다.

---

## 9. NVIC 인터럽트 설정

USART1과 USART6의 **NVIC Settings**에서 각 USART Global Interrupt를 활성화한다. DMA 인터럽트는 DMA 추가 시 함께 활성화되었는지 확인한다.

최종 우선순위는 다음과 같다.

| 인터럽트 | Preemption Priority | Sub Priority |
|---|---:|---:|
| TIM5 global interrupt | 0 | 0 |
| USART1 global interrupt | 5 | 0 |
| USART6 global interrupt | 5 | 0 |
| DMA2 Stream 1 global interrupt | 5 | 0 |
| DMA2 Stream 5 global interrupt | 5 | 0 |
| DMA2 Stream 6 global interrupt | 6 | 0 |
| DMA2 Stream 7 global interrupt | 6 | 0 |

### 9.1 인터럽트 우선순위 해석

NVIC에서는 숫자가 작을수록 인터럽트 우선순위가 높다.

```text
Priority 0 > Priority 5 > Priority 6 > Priority 15
```

- RX와 USART 인터럽트는 수신 손실을 줄이기 위해 우선순위 5를 사용한다.
- TX DMA는 RX보다 낮은 우선순위인 6을 사용한다.
- TIM5는 HAL Timebase 용도이며 CubeMX가 생성한 우선순위 0을 유지한다.
- FreeRTOS API를 인터럽트 안에서 호출하려면 해당 인터럽트의 NVIC 우선순위가 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`의 허용 범위에 있어야 한다.

FreeRTOS 설정값은 다음을 기준으로 한다.

```text
configLIBRARY_LOWEST_INTERRUPT_PRIORITY       = 15
configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5
```

따라서 우선순위 5와 6인 USART·DMA 인터럽트에서는 `...FromISR()` 계열 FreeRTOS API를 사용할 수 있다. 우선순위 0인 TIM5 인터럽트에서는 FreeRTOS API를 호출하지 않는다.

---

## 10. FreeRTOS 설정

### 10.1 FreeRTOS 활성화

메뉴 경로:

```text
Pinout & Configuration
→ Middleware and Software Packs
→ FreeRTOS
```

| 항목 | 값 |
|---|---|
| Interface | CMSIS_V2 |
| Memory Management Scheme | heap_4 |
| TOTAL_HEAP_SIZE | 32768 bytes 권장 |
| MAX_TASK_NAME_LEN | 24 |

`MAX_TASK_NAME_LEN`은 `InputControlTask`와 `SortingControlTask` 이름이 디버거에서 잘리지 않도록 24로 설정한다.

### 10.2 태스크 생성

메뉴 경로:

```text
FreeRTOS
→ Tasks and Queues
→ Tasks
```

현재 생성할 태스크는 다음과 같다.

| Task Name | Priority | Stack Size | Entry Function | Code Generation |
|---|---|---:|---|---|
| `CommRxTask` | AboveNormal | 512 words | `StartCommRxTask` | As weak |
| `InputControlTask` | Normal | 512 words | `StartInputControlTask` | As weak |
| `SortingControlTask` | Normal | 512 words | `StartSortingControlTask` | As weak |

CubeMX가 기본 생성한 `defaultTask`는 초기 단계에서는 삭제하지 않아도 된다.

```text
defaultTask: Low, Stack 128
```

### 10.3 FreeRTOS 태스크 우선순위 해석

FreeRTOS 태스크 우선순위는 NVIC와 달리 높은 단계의 이름이 더 높은 실행 우선순위를 뜻한다.

```text
Normal < Normal1 < ... < Normal7 < AboveNormal
```

현재는 두 Control 태스크 모두 숫자 없는 `Normal`을 선택한다. 같은 우선순위의 실행 가능한 태스크는 FreeRTOS 스케줄러가 Time Slicing 방식으로 번갈아 실행한다.

`CommRxTask`는 UART 데이터를 빠르게 처리하고 다른 태스크에 전달해야 하므로 `AboveNormal`을 사용한다.

### 10.4 태스크 역할

#### CommRxTask

- USART1·USART6 수신 DMA와 UART 이벤트 처리
- 프레임 검사 및 메시지 파싱
- 투입 명령을 `InputControlTask`에 전달
- 서버 처리 결과로 생성된 분류 장치 제어 명령을 `SortingControlTask`에 전달
- 정지·E-Stop·Reset 명령을 Safety 처리 경로에 전달

#### InputControlTask

- 컨베이어 1의 시작·정지·속도 제어
- 현재 동작 상태와 모터 오류 상태 관리
- `GET_STATUS` 요청에 사용할 상태 응답 데이터 생성

#### SortingControlTask

- `sortingControlQueue`에서 서버 처리 결과로 생성된 제어 명령 소비
- 컨베이어 2 시작·정지·속도 제어와 상태 응답 생성
- `ROUTE_ITEM(cycleId, destination)`을 목적지별 MG90S PWM으로 변환
- `RETURN_HOME(cycleId)` 일치 검증 후 비블로킹 Home 복귀
- Home 정착 완료 후 `CYCLE_COMPLETE(cycleId, destination)` 송신
- 동일 sequence 응답 캐시, 중복 실행 방지와 TX 큐 재시도
- Safety epoch를 이용한 E-Stop 이전 명령 폐기와 게이트 Home 동기화

---

## 11. Project Manager 설정

### 11.1 Project 탭

| 항목 | 값 |
|---|---|
| Project Name | `integrated_conveyor_controller` |
| Application Structure | Advanced |
| Toolchain / IDE | STM32CubeIDE |
| Generate Under Root | Enable |
| Do not generate the main() | Disable |
| Minimum Heap Size | `0x200` |
| Minimum Stack Size | `0x400` |

### 11.2 Firmware Package

| 항목 | 값 |
|---|---|
| Firmware Package | STM32Cube FW_F4 V1.28.3 |
| Use Default Firmware Location | Enable |
| Use latest available version | Disable 권장 |

빌드 재현성을 위해 프로젝트 생성 시 사용한 펌웨어 패키지 버전을 고정한다.

### 11.3 Code Generator 탭

| 항목 | 설정 |
|---|---|
| Copy only the necessary library files | 선택 |
| Generate peripheral initialization as a pair of `.c/.h` files per peripheral | 선택 권장 |
| Backup previously generated files when re-generating | 선택 사항 |
| Keep User Code when re-generating | 선택 |
| Delete previously generated files when not re-generated | 선택 |
| Enable Full Assert | 초기 단계에서는 선택하지 않음 |

사용자가 직접 작성한 코드는 가능한 한 CubeMX의 다음 영역 안에 작성한다.

```c
/* USER CODE BEGIN ... */

/* USER CODE END ... */
```

더 큰 기능은 별도의 `Application` 소스 파일에 구현하여 CubeMX 재생성의 영향을 받지 않도록 한다.

---

## 12. 코드 생성

1. `.ioc` 파일을 저장한다.
2. 우측 상단의 **GENERATE CODE**를 누른다.
3. `USE_NEWLIB_REENTRANT` 경고가 표시되면 현재 단계에서는 **Yes**를 눌러 코드 생성을 계속한다.

경고 내용은 여러 태스크가 동시에 newlib의 비재진입 함수를 사용할 때 문제가 발생할 수 있다는 의미다. 현재 태스크에서 `printf`, `sprintf`, 동적 메모리 할당 등을 동시에 사용하지 않는다면 `USE_NEWLIB_REENTRANT`를 즉시 활성화하지 않아도 된다.

향후 여러 태스크에서 newlib 함수를 사용해야 한다면 다음 메뉴에서 활성화한다.

```text
FreeRTOS
→ Advanced Settings
→ USE_NEWLIB_REENTRANT
→ Enabled
```

활성화하면 태스크별 newlib 상태 구조가 추가되어 RAM 사용량이 증가한다.

---

## 13. STM32CubeIDE에서 생성 결과 확인

### 13.1 소스 코드 확인

Project Explorer에서 다음 파일을 확인한다.

```text
Core/Src/freertos.c
```

CubeMX 버전에 따라 다음 파일에 생성될 수도 있다.

```text
Core/Src/app_freertos.c
```

IDE에서 `Ctrl + H`를 누르고 **File Search**를 선택하여 다음 이름을 검색한다.

```text
CommRxTask
InputControlTask
SortingControlTask
```

다음과 같은 핸들, 속성, 생성 코드, 진입 함수가 존재해야 한다.

```c
osThreadId_t CommRxTaskHandle;
osThreadId_t InputControlTaskHandle;
osThreadId_t SortingControlTaskHandle;
```

```c
CommRxTaskHandle = osThreadNew(
    StartCommRxTask,
    NULL,
    &CommRxTask_attributes);
```

```c
void StartCommRxTask(void *argument)
{
    for (;;)
    {
        osDelay(1);
    }
}
```

Input 및 Sorting 태스크도 같은 구조로 생성되어야 한다.

### 13.2 빌드 확인

STM32CubeIDE에서 다음 메뉴를 실행한다.

```text
Project
→ Build Project
```

Console에 오류가 없고 `.elf` 파일이 생성되면 CubeMX와 FreeRTOS 기본 설정이 정상적으로 빌드된 것이다.

### 13.3 실행 중 태스크 확인

보드에 펌웨어를 다운로드하고 Debug 실행 후 다음 View를 연다.

```text
Window
→ Show View
→ Other...
→ FreeRTOS
→ Task List
```

`osKernelStart()`가 실행된 이후 다음 태스크들의 상태, 우선순위, 스택 사용량을 확인할 수 있다.

```text
CommRxTask
InputControlTask
SortingControlTask
defaultTask
```

CubeIDE 버전에 따라 FreeRTOS Task List의 메뉴 위치나 이름이 다를 수 있다. 이 경우 Debug 화면의 RTOS 또는 FreeRTOS 관련 View를 사용한다.

---

## 14. 코드 생성 후 기본 점검표

### 하드웨어 및 Clock

- [ ] 보드가 NUCLEO-F401RE로 선택되어 있다.
- [ ] HSE가 BYPASS Clock Source로 설정되어 있다.
- [ ] SYSCLK가 84 MHz다.
- [ ] SYS Debug가 Serial Wire다.
- [ ] HAL Timebase Source가 TIM5다.

### UART 및 DMA

- [ ] USART1 핀이 PA9·PA10이다.
- [ ] USART6 핀이 PA11·PA12다.
- [ ] 두 USART가 115200, 8-N-1로 동일하게 설정되어 있다.
- [ ] USART1_RX와 USART6_RX DMA가 Circular 모드다.
- [ ] USART1_TX와 USART6_TX DMA가 Normal 모드다.
- [ ] USART1·USART6 Global Interrupt가 활성화되어 있다.
- [ ] UART TX/RX가 Raspberry Pi와 교차 연결되어 있다.
- [ ] STM32와 Raspberry Pi의 GND가 공통 연결되어 있다.

### FreeRTOS

- [ ] CMSIS_V2가 선택되어 있다.
- [ ] Memory Management가 heap_4다.
- [ ] `MAX_TASK_NAME_LEN`이 24다.
- [ ] `CommRxTask`가 AboveNormal, Stack 512다.
- [ ] `InputControlTask`가 Normal, Stack 512다.
- [ ] `SortingControlTask`가 Normal, Stack 512다.
- [ ] 세 태스크의 Code Generation Option이 `As weak`다.

### 코드 생성 및 빌드

- [ ] `Keep User Code when re-generating`이 활성화되어 있다.
- [ ] 세 태스크의 핸들과 진입 함수가 생성되었다.
- [ ] STM32CubeIDE 빌드가 오류 없이 완료된다.
- [ ] Debug 실행 중 FreeRTOS Task List에서 세 태스크가 확인된다.

---

## 15. 다음 구현 순서

기본 설정과 빌드 확인이 끝나면 다음 순서로 구현한다.

1. USART1·USART6 DMA 수신 버퍼 정의
2. UART Idle Line 또는 수신 이벤트 기반 프레임 추출
3. UART 패킷 구조 및 CRC 검증 구현
4. `CommRxTask` 메시지 파싱 구현
5. Control 태스크로 전달할 Queue 메시지 타입 정의
6. `InputControlTask` 상태 머신 구현
7. `SortingControlTask` 상태 머신 구현
8. 모터 PWM·GPIO 출력 구현
9. Raspberry Pi 한 대로 USART 단일 채널 통신 시험
10. Raspberry Pi 두 대를 연결한 USART1·USART6 통합 시험

처음에는 UART 한 채널만 시험하더라도 USART1과 USART6의 CubeMX 설정은 모두 유지한다. 통신 시험 코드는 채널별로 독립적으로 검증한 뒤 두 채널을 동시에 실행한다.
