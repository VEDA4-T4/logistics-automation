# STM32CubeIDE 프로젝트 Git 연동 및 공용 UART 코드 통합 매뉴얼

## 1. 문서 목적

이 문서는 [STM32F401RE FreeRTOS 설정 매뉴얼](./stm32f401re-freertos-setup.md)의 다음 단계부터, 현재 `CommRxTask` 구현을 시작하기 직전까지 진행한 작업을 정리한다.

주요 범위는 다음과 같다.

- 생성한 STM32CubeIDE 프로젝트를 Git 저장소 내부로 배치
- STM32CubeIDE에서 기존 프로젝트 Import
- Git 기능별 브랜치 구성
- 저장소의 공용 UART 프로토콜 코드 반영
- STM32 프로젝트에서 공용 Header와 Source 연결
- CubeIDE 인덱스 재생성 및 전체 빌드 검증

> 현재는 UART 프로토콜·CRC·Codec·Parser 코드가 STM32 빌드에 포함된 상태이다. UART DMA 수신 콜백, RTOS Queue, `CommRxTask` 명령 분배 로직은 아직 구현 전이다.

---

## 2. 현재 저장소와 프로젝트 경로

Git 저장소 루트는 다음과 같다.

```text
C:\Users\3-20\OneDrive\Desktop\logistics-automation
```

STM32 프로젝트의 실제 경로는 다음과 같다.

```text
C:\Users\3-20\OneDrive\Desktop\logistics-automation\stm32\conveyor-controller
```

주요 디렉터리 구조는 다음과 같다.

```text
logistics-automation/
├─ docs/
├─ shared/
│  ├─ include/logistics/contracts/
│  │  ├─ uart_protocol.h
│  │  ├─ uart_codec.h
│  │  ├─ uart_crc16.h
│  │  ├─ uart_parser.h
│  │  └─ uart/
│  │     ├─ input_commands.h
│  │     └─ sorting_commands.h
│  └─ contracts/uart/
│     ├─ uart_codec.c
│     ├─ uart_crc16.c
│     └─ uart_parser.c
└─ stm32/
   └─ conveyor-controller/
      ├─ Core/
      ├─ Drivers/
      ├─ Middlewares/
      ├─ integrated_conveyor_controller.ioc
      ├─ .project
      └─ .cproject
```

CubeIDE 화면에 표시되는 프로젝트 이름은 다음과 같다.

```text
integrated_conveyor_controller
```

실제 디렉터리 이름인 `conveyor-controller`와 CubeIDE 내부 프로젝트 이름인 `integrated_conveyor_controller`가 서로 다른 것은 정상이다.

---

## 3. STM32 프로젝트를 Git 저장소에 배치

### 3.1 프로젝트 위치 원칙

CubeMX에서 생성한 프로젝트는 반드시 Git 저장소 내부의 다음 위치에 있어야 한다.

```text
logistics-automation/stm32/conveyor-controller
```

STM32CubeIDE의 Workspace는 소스 저장소가 아니라 IDE 설정과 프로젝트 목록을 보관하는 공간이다. 따라서 다음 두 경로를 구분한다.

| 구분 | 경로 | 역할 |
|---|---|---|
| Git 저장소 | `C:\Users\3-20\OneDrive\Desktop\logistics-automation` | 실제 소스 및 Git 변경 이력 |
| CubeIDE Workspace | `C:\Users\3-20\STM32CubeIDE\workspace_logistics` | IDE 설정 및 프로젝트 참조 정보 |

Workspace를 변경해도 Git 브랜치가 바뀌거나 소스가 자동 복사되는 것은 아니다. 실제로 Git에 반영되는 파일은 저장소 내부의 `stm32/conveyor-controller` 파일이다.

### 3.2 프로젝트 이동 시 주의사항

프로젝트를 다른 위치에서 저장소로 옮길 때는 프로젝트 폴더 전체를 옮긴다.

필수 파일과 폴더의 예시는 다음과 같다.

```text
.project
.cproject
integrated_conveyor_controller.ioc
Core/
Drivers/
Middlewares/
STM32F401RETX_FLASH.ld
STM32F401RETX_RAM.ld
```

`Debug/`, `Release/`, `.metadata/`는 다시 생성할 수 있는 로컬 산출물이므로 Git에 포함하지 않는다.

---

## 4. STM32CubeIDE에 프로젝트 Import

### 4.1 별도 Workspace 열기

STM32CubeIDE 실행 시 Workspace 선택 창에서 다음 경로를 선택한다.

```text
C:\Users\3-20\STM32CubeIDE\workspace_logistics
```

그다음 **Launch**를 누른다.

### 4.2 기존 프로젝트 불러오기

CubeIDE에서 다음 메뉴로 이동한다.

```text
File
└─ Import...
   └─ General
      └─ Existing Projects into Workspace
```

설정값은 다음과 같다.

| 항목 | 설정 |
|---|---|
| Select root directory | `C:\Users\3-20\OneDrive\Desktop\logistics-automation\stm32\conveyor-controller` |
| Projects | `integrated_conveyor_controller` 선택 |
| Copy projects into workspace | 선택하지 않음 |
| Search for nested projects | 선택 가능 |

**Copy projects into workspace**를 선택하지 않아야 CubeIDE가 Git 저장소 내부의 원본 프로젝트를 직접 사용한다.

### 4.3 `already exist in the workspace` 오류 처리

다음 오류가 표시될 수 있다.

```text
Some projects cannot be imported because they already exist in the workspace
```

이 메시지는 같은 내부 이름의 프로젝트가 현재 Workspace에 이미 등록되어 있다는 뜻이다. 소스가 손상되었다는 의미는 아니다.

처리 방법은 다음 두 가지 중 하나이다.

1. Project Explorer에 기존 프로젝트가 정상적으로 보이면 Import를 취소하고 기존 항목을 사용한다.
2. 기존 항목이 잘못된 경로를 가리키면 Project Explorer에서 프로젝트를 제거한 뒤 다시 Import한다.

프로젝트를 제거할 때 **Delete project contents on disk**는 선택하지 않는다. 이 옵션을 선택하면 실제 Git 저장소의 파일까지 삭제될 수 있다.

### 4.4 Import 결과 확인

Project Explorer에 다음 구조가 보이면 Import가 완료된 것이다.

```text
integrated_conveyor_controller
├─ Binaries
├─ Includes
├─ Core
├─ Drivers
├─ Middlewares
└─ integrated_conveyor_controller.ioc
```

---

## 5. Git 브랜치 구성

기능 하나당 브랜치 하나를 사용하는 방식으로 작업한다.

현재 사용한 브랜치 구분은 다음과 같다.

| 브랜치 | 목적 |
|---|---|
| `main` | 검증 및 병합이 완료된 기준 코드 |
| `feature/conveyor-controller/base` | STM32CubeMX·FreeRTOS 초기 설정과 공통 기반 |
| `feature/conveyor-controller/comm-rx-task` | `base`에서 분기한 VEDA-168 `CommRxTask` 구현 |

초기 프로젝트 설정과 UART 공용 코드는 `feature/conveyor-controller/base`에서 관리하며 아직 `main`에 병합하지 않는다. `CommRxTask` 브랜치는 최신 `base`에서 분기하여 CubeIDE의 공용 UART Source 연결과 수신 Task 구현을 담당한다.

현재 브랜치 확인 명령은 다음과 같다.

```powershell
git branch --show-current
git status -sb
```

정상적인 현재 브랜치는 다음과 같다.

```text
feature/conveyor-controller/comm-rx-task
```

### 5.1 CubeIDE와 Git 브랜치의 관계

CubeIDE 프로젝트가 Git 저장소 내부의 원본 파일을 직접 참조하고 있다면, CubeIDE에서 저장한 변경 사항은 현재 체크아웃된 Git 브랜치의 변경 사항이 된다.

즉, CubeIDE에 별도로 브랜치를 연결하는 설정은 필요 없다. 터미널에서 브랜치를 전환한 후 CubeIDE에서 프로젝트를 새로고침하면 된다.

```text
Project 우클릭
└─ Refresh
```

브랜치 전환 전에는 저장하지 않은 CubeIDE 편집 내용을 먼저 저장하고, `git status`로 변경 파일을 확인한다.

---

## 6. STM32 System Tick 경고 처리

프로젝트를 다시 열 때 다음 경고가 표시되었다.

```text
Incorrect preemption priority for system tick timer '0'.
Do you want to fix it and set it to 15?
```

이 경우 **Yes**를 선택하여 System Tick 우선순위를 `15`로 변경한다.

현재 우선순위 기준은 다음과 같다.

| 대상 | Preemption Priority |
|---|---:|
| TIM5 HAL Timebase | 0 |
| USART1·USART6 및 RX DMA | 5 |
| TX DMA | 6 |
| System Tick | 15 |

System Tick은 FreeRTOS에서 사용하므로 `configLIBRARY_LOWEST_INTERRUPT_PRIORITY` 값인 `15`에 맞춘다. HAL Timebase는 기존 설정대로 TIM5를 사용한다.

---

## 7. 공용 UART 코드 반영

### 7.1 공용 코드의 역할

STM32와 Raspberry Pi가 같은 패킷 규칙을 사용하도록 UART 규약을 `shared` 디렉터리에서 공용으로 관리한다.

현재 패킷 구조는 다음과 같다.

```text
SOF | VERSION | SEQUENCE | COMMAND | LENGTH | PAYLOAD | CRC16
```

주요 값은 다음과 같다.

| 항목 | 값 |
|---|---|
| SOF | `0xAA` |
| Protocol Version | `0x01` |
| 최대 Payload | 128 bytes |
| CRC | CRC16-CCITT-FALSE |

### 7.2 공용 Header

다음 Header를 STM32와 Raspberry Pi 코드가 함께 사용한다.

| 파일 | 역할 |
|---|---|
| `uart_protocol.h` | 공통 Frame, 명령 범위, 상태 및 오류 정의 |
| `uart_codec.h` | 완성된 Frame 인코딩·디코딩 API |
| `uart_crc16.h` | CRC16 계산 API |
| `uart_parser.h` | Byte 단위 스트림 Parser 상태와 API |
| `uart/input_commands.h` | 투입 컨베이어 명령 정의 |
| `uart/sorting_commands.h` | 분류 컨베이어 및 게이트 명령 정의 |

### 7.3 공용 Source

STM32 빌드에 포함되는 구현 파일은 다음과 같다.

```text
shared/contracts/uart/uart_codec.c
shared/contracts/uart/uart_crc16.c
shared/contracts/uart/uart_parser.c
```

각 파일의 역할은 다음과 같다.

- `uart_codec.c`: 한 개의 완성된 Frame을 Byte 배열로 인코딩하거나 Byte 배열에서 디코딩
- `uart_crc16.c`: 패킷 무결성 검사용 CRC16 계산
- `uart_parser.c`: DMA로 나뉘어 들어오는 Byte 스트림에서 완전한 Frame 추출

### 7.4 현재 명령 ID

투입 장치 명령은 다음과 같다.

| 명령 | ID |
|---|---:|
| `UART_CMD_CONVEYOR_START` | `0x10` |
| `UART_CMD_CONVEYOR_STOP` | `0x11` |
| `UART_CMD_CONVEYOR_SET_SPEED` | `0x12` |
| `UART_CMD_CONVEYOR_GET_STATUS` | `0x13` |
| `UART_CMD_CONTROL_RESET` | `0x14` |

분류 장치 명령은 다음과 같다.

| 명령 | ID |
|---|---:|
| `UART_CMD_SORTING_ROUTE_ITEM` | `0x30` |
| `UART_CMD_SORTING_GET_STATUS` | `0x31` |
| `UART_CMD_SORTING_CANCEL` | `0x32` |
| `UART_CMD_SORTING_RESET` | `0x33` |
| `UART_CMD_SORTING_CONVEYOR_START` | `0x34` |
| `UART_CMD_SORTING_CONVEYOR_STOP` | `0x35` |
| `UART_CMD_SORTING_CONVEYOR_SET_SPEED` | `0x36` |
| `UART_CMD_SORTING_CONVEYOR_GET_STATUS` | `0x37` |
| `UART_CMD_SORTING_RETURN_HOME` | `0x38` |

정상 분류 cycle의 게이트 명령 계약은 다음과 같다.

```text
SORTING_ROUTE_ITEM payload = [cycle_id_low, cycle_id_high, destination_id]
SORTING_RETURN_HOME payload = [cycle_id_low, cycle_id_high]

서버/Raspberry Pi → ROUTE_ITEM(cycleId, destination)
STM32             → 목적지에 대응하는 게이트 위치로 이동
서버/Raspberry Pi → 물품 통과 판정
서버/Raspberry Pi → RETURN_HOME(cycleId)
STM32             → Home 복귀 후 CYCLE_COMPLETE(cycleId, destination) 송신
```

`cycleId`는 `1..65535` 범위를 사용하며 `0`은 활성 cycle이 없음을 나타내는 예약값이다.

`RETURN_HOME`은 정상 공정 완료용이고 `CANCEL`은 비정상 중단용이다. 명령은 UART
계약과 RX 검증·큐 라우팅을 거쳐 `SortingControlTask`에서 처리한다.
`RETURN_HOME`의 `OPERATION_RESULT/SUCCESS`는 Home 이동 명령이 수락되었다는 뜻이며,
실제 서보 정착 완료는 이후 `CYCLE_COMPLETE` 이벤트로 확정한다.

공통 명령에는 `UART_CMD_PING`(`0x01`)과 `UART_CMD_EMERGENCY_STOP`(`0xF0`) 등이 있다.

---

## 8. CubeIDE에 공용 Header 경로 추가

공용 Header가 STM32 프로젝트 외부의 `shared/include`에 있으므로 컴파일러 Include Path를 추가했다.

프로젝트를 우클릭하고 다음 메뉴로 이동한다.

```text
Properties
└─ C/C++ Build
   └─ Settings
      └─ Tool Settings
         └─ MCU GCC Compiler
            └─ Include paths
```

다음 경로를 추가한다.

```text
${ProjDirPath}/../../shared/include
```

Debug와 Release 설정에 모두 같은 경로를 추가한다.

| Configuration | Include Path |
|---|---|
| Debug | `${ProjDirPath}/../../shared/include` |
| Release | `${ProjDirPath}/../../shared/include` |

경로를 추가한 후 **Apply and Close**를 누른다.

소스에서는 다음과 같이 공용 Header를 사용할 수 있다.

```c
#include "logistics/contracts/uart_protocol.h"
#include "logistics/contracts/uart_parser.h"
#include "logistics/contracts/uart/input_commands.h"
#include "logistics/contracts/uart/sorting_commands.h"
```

---

## 9. CubeIDE에 공용 UART Source 연결

Include Path만 추가하면 Header는 찾을 수 있지만, `uart_codec.c`, `uart_crc16.c`, `uart_parser.c`는 자동으로 컴파일되지 않는다. 따라서 CubeIDE 프로젝트에 Linked Folder를 추가했다.

프로젝트의 `.project` 파일에는 다음 Linked Resource가 등록되어 있다.

```xml
<link>
    <name>SharedUart</name>
    <type>2</type>
    <locationURI>PARENT-2-PROJECT_LOC/shared/contracts/uart</locationURI>
</link>
```

이 설정은 다음 실제 디렉터리를 CubeIDE Project Explorer의 `SharedUart` 폴더로 연결한다.

```text
logistics-automation/shared/contracts/uart
```

또한 `.cproject`의 Debug와 Release Source Entry에 `SharedUart`를 추가했다.

```xml
<entry flags="VALUE_WORKSPACE_PATH|RESOLVED"
       kind="sourcePath"
       name="SharedUart"/>
```

CubeIDE에서 프로젝트를 새로고침하면 다음 항목이 보여야 한다.

```text
integrated_conveyor_controller
└─ SharedUart
   ├─ uart_codec.c
   ├─ uart_crc16.c
   └─ uart_parser.c
```

> `SharedUart`는 파일 복사본이 아니다. 저장소의 `shared/contracts/uart` 원본 디렉터리를 가리키므로 어느 쪽에서 수정해도 같은 파일이 변경된다.

---

## 10. CubeIDE 인덱스 재생성

Include Path를 변경하고 **Apply and Close**를 누르면 다음 메시지가 표시될 수 있다.

```text
Some build settings changes may affect the index.
Do you wish to rebuild it now?
```

이 경우 **Rebuild Index**를 선택한다.

인덱스 재생성은 코드 자동 완성, Header 탐색, 오류 표시를 새 Include Path에 맞게 갱신하는 작업이다. 실제 컴파일 설정을 대신하는 것은 아니므로 인덱스 재생성 후 반드시 전체 빌드도 수행한다.

수동으로 다시 실행하려면 다음 메뉴를 사용한다.

```text
Project 우클릭
└─ Index
   └─ Rebuild
```

---

## 11. 로컬 빌드 산출물 Git 제외

CubeIDE에서 생성되는 로컬 빌드 파일과 컴퓨터별 인덱스 설정은 Git에 올리지 않는다.

`.gitignore`에 다음 항목을 추가했다.

```gitignore
# STM32CubeIDE local build and workspace artifacts
stm32/**/Debug/
stm32/**/Release/
stm32/**/.metadata/
stm32/**/.settings/language.settings.xml
```

Git에 포함해야 하는 파일은 다음과 같다.

- `.project`: `SharedUart` Linked Folder 설정 포함
- `.cproject`: Include Path와 Source Entry 설정 포함
- `.ioc`: CubeMX 하드웨어 및 FreeRTOS 설정
- `Core`, `Drivers`, `Middlewares`의 실제 소스
- `shared/include` 및 `shared/contracts/uart` 공용 코드

---

## 12. 빌드 검증

### 12.1 CubeIDE에서 빌드

프로젝트를 선택하고 다음 메뉴를 실행한다.

```text
Project
└─ Clean...
```

그다음 다음 메뉴를 실행한다.

```text
Project
└─ Build Project
```

Console에서 다음 공용 소스들이 컴파일되는지 확인한다.

```text
SharedUart/uart_codec.c
SharedUart/uart_crc16.c
SharedUart/uart_parser.c
```

최종 확인 결과는 다음과 같다.

```text
0 errors, 0 warnings
```

즉, 다음 항목이 모두 검증되었다.

- STM32 기본 프로젝트 컴파일 성공
- FreeRTOS 및 HAL 코드 컴파일 성공
- `shared/include` Header 탐색 성공
- 공용 UART Source 컴파일 성공
- 최종 ELF 링크 성공

### 12.2 Header 오류가 남아 있을 때

빌드는 성공하지만 편집기에 빨간 밑줄만 남아 있다면 인덱스 문제일 가능성이 높다.

다음 순서로 처리한다.

1. 프로젝트 우클릭 후 **Refresh**
2. 프로젝트 우클릭 후 **Index → Rebuild**
3. **Project → Clean**
4. **Project → Build Project**

실제 Console 빌드도 실패한다면 다음 경로가 Debug와 Release에 모두 들어 있는지 확인한다.

```text
${ProjDirPath}/../../shared/include
```

### 12.3 공용 Source가 컴파일되지 않을 때

Project Explorer에서 `SharedUart` 폴더가 보이는지 확인한다. 보이지 않으면 다음 항목을 점검한다.

- `.project`에 `SharedUart` Linked Resource가 있는가
- Linked Resource가 `PARENT-2-PROJECT_LOC/shared/contracts/uart`를 가리키는가
- `.cproject`의 Debug와 Release Source Entry에 `SharedUart`가 있는가
- 프로젝트를 Refresh했는가

---

## 13. 현재 완료 상태

현재까지 완료된 작업은 다음과 같다.

- [x] STM32F401RE CubeMX 및 FreeRTOS 초기 설정
- [x] USART1·USART6 및 RX/TX DMA 설정
- [x] `CommRxTask`, `InputControlTask`, `SortingControlTask` 생성
- [x] `SafetyTask`, `CommTxTask`, `SensorTask`, `HealthTask` 생성
- [x] STM32 프로젝트를 Git 저장소에 배치
- [x] 별도 CubeIDE Workspace에서 원본 프로젝트 Import
- [x] `feature/conveyor-controller/base` 기반 브랜치 준비
- [x] VEDA-168 기능 브랜치 준비
- [x] base에 공용 UART Protocol, CRC16, Codec, Parser 코드 반영
- [x] base에 투입 및 분류 명령 계약 반영
- [x] 공용 Header Include Path 연결
- [x] 공용 UART Source Linked Folder 연결
- [x] CubeIDE 인덱스 재생성
- [x] 전체 Debug 빌드 성공: 오류 0, 경고 0
- [ ] UART DMA 수신 시작 코드 구현
- [ ] UART 수신 이벤트 콜백 구현
- [ ] ISR에서 `CommRxTask`로 전달할 Queue 구현
- [ ] `CommRxTask` Parser 연결 및 Frame 검증
- [ ] Input·Sorting Control Queue 분배 구현
- [ ] Dummy Frame 수신 시험
- [ ] Raspberry Pi 실제 통신 시험

---

## 14. 다음 구현 순서

현재 설정 이후에는 다음 순서로 구현한다.

1. `Application/Inc`, `Application/Src` 디렉터리 생성
2. UART 수신 Chunk 구조체 정의
3. ISR에서 `CommRxTask`로 전달할 `UartRxChunkQueue` 생성
4. USART1 `HAL_UARTEx_ReceiveToIdle_DMA()` 수신 시작
5. `HAL_UARTEx_RxEventCallback()`에서 수신 Byte를 Queue로 전달
6. `CommRxTask`에서 `uart_parser_feed()` 호출
7. 완성된 Frame의 CRC, Version, Command 검증
8. 투입 명령은 `InputControlQueue`로 전달
9. 분류 명령은 `SortingControlQueue`로 전달
10. Dummy Frame으로 명령 분배 결과 확인
11. USART1과 Raspberry Pi 1대로 실제 시험
12. USART6까지 확장하여 Raspberry Pi 2대 통합 시험

첫 번째 시험 목표는 모터를 바로 동작시키는 것이 아니다. 다음 데이터 흐름이 정상인지 카운터, 디버거 변수 또는 Breakpoint로 확인하는 것이다.

```text
USART1 Dummy Frame
    → RX DMA
    → UART RX Event Callback
    → UartRxChunkQueue
    → CommRxTask
    → UART Parser 및 CRC 검증
    → Command 판별
    → InputControlQueue 또는 SortingControlQueue
```

---

## 15. 최종 확인 체크리스트

### Git 및 프로젝트 위치

- [ ] 터미널의 현재 위치가 `logistics-automation` 저장소인가
- [ ] 현재 브랜치가 `feature/conveyor-controller/comm-rx-task`인가
- [ ] STM32 프로젝트가 `stm32/conveyor-controller`에 있는가
- [ ] CubeIDE의 **Copy projects into workspace**를 사용하지 않았는가

### CubeIDE 연결

- [ ] Project Explorer에 `integrated_conveyor_controller`가 보이는가
- [ ] `SharedUart` 폴더가 보이는가
- [ ] Debug Include Path에 `${ProjDirPath}/../../shared/include`가 있는가
- [ ] Release Include Path에도 같은 경로가 있는가
- [ ] 인덱스를 Rebuild했는가

### 빌드

- [ ] `uart_codec.c`가 컴파일되는가
- [ ] `uart_crc16.c`가 컴파일되는가
- [ ] `uart_parser.c`가 컴파일되는가
- [ ] 최종 빌드가 오류 0, 경고 0으로 끝나는가

### 구현 시작 전 상태

- [ ] 공용 UART 코드 자체와 STM32 연결 설정을 구분하고 있는가
- [ ] DMA 콜백에서 Parser나 모터 제어를 직접 실행하지 않을 계획인가
- [ ] USART1 한 채널 Dummy 시험부터 시작할 계획인가
- [ ] USART1과 USART6에 각각 독립적인 Parser 상태를 둘 계획인가
