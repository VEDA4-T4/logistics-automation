# 프로젝트 구조

## 실행 구성

| 구성 요소 | 실행 환경 | 책임 |
| --- | --- | --- |
| Control Center | Windows 또는 Ubuntu, Qt 6.10+ | 공정 제어, 장치 상태, 작업 이력, RTSP/ONVIF 영상 |
| Mosquitto | 중앙 Raspberry Pi 또는 별도 서버 | MQTT 메시지 브로커 |
| Central Server | 중앙 Raspberry Pi | 메시지 검증·저장·라우팅, 공정 상태 머신, HTTP(S) 업로드 |
| Device Node | 공정별 Raspberry Pi | MQTT와 UART 변환, 상태·heartbeat 보고 |
| STM32 Controller | 공정별 STM32 | 센서, 모터, 안전 제어 |

## 저장소 구조

```text
logistics-automation/
├─ CMakeLists.txt
├─ cmake/                         # 공통 빌드 정책
├─ control-center/                # Qt 관제 시스템
├─ central-server-rpi/            # 중앙서버
│  ├─ config/
│  ├─ db/migrations/
│  ├─ include/
│  ├─ src/
│  └─ tests/
├─ device-rpi/                    # Input/Vision/Sorting/Line Tracer 노드
│  ├─ common/
│  ├─ config/
│  ├─ input-node/
│  ├─ vision-node/
│  ├─ sorting-node/
│  ├─ linetracer-node/
│  └─ kernel/vedauart/
├─ stm32/                         # 장치 제어 펌웨어
│  ├─ input-controller/
│  ├─ gripper-controller/
│  ├─ sorting-controller/
│  └─ linetracer-controller/
├─ shared/                        # 공통 계약·코덱
├─ deploy/                        # 설치·서비스·브로커 운영
├─ docs/                          # 프로젝트 가이드
└─ runtime/                       # 로컬 설정·DB·업로드 파일, Git 제외
```

현재 `device-rpi`의 Input, Sorting, Line Tracer 실행 파일은 공통 `NodeRuntime` 기반입니다. Vision은 카메라·바코드
인식과 이미지 업로드가 구현되어 있습니다. Gripper의 중앙 공정 계약과 STM32 폴더는 존재하지만 전용 Raspberry Pi
실행 파일은 아직 없습니다.

## 의존성 규칙

1. Control Center는 장치나 SQLite에 직접 접근하지 않고 중앙서버와 MQTT·HTTP로 통신합니다.
2. PC/Raspberry Pi 사이는 MQTT와 HTTP(S), Device Raspberry Pi/STM32 사이는 UART를 사용합니다.
3. 통신 필드 변경은 먼저 `shared/contracts`와 공통 헤더에 반영합니다.
4. 실시간 센서·모터·안전 제어는 STM32가 담당하고 Raspberry Pi는 명령 변환과 상위 상태 보고를 담당합니다.
5. 비상정지는 자동 해제하지 않으며 명시적인 복구 절차를 거칩니다.
6. 인증 정보와 장치별 주소는 코드에 넣지 않고 런타임 INI로 주입합니다.

## 주요 빌드 옵션

| 옵션 | 기본값 | 설명 |
| --- | --- | --- |
| `LOGISTICS_BUILD_CONTROL_CENTER` | `OFF` | Qt 관제 시스템 |
| `LOGISTICS_BUILD_CENTRAL_SERVER` | `ON` | 중앙서버 |
| `LOGISTICS_BUILD_DEVICE_NODES` | `ON` | Raspberry Pi 장치 노드 |
| `LOGISTICS_ENABLE_MOSQUITTO_TRANSPORT` | `ON` | 실제 libmosquitto 전송 |

STM32 프로젝트는 최상위 CMake 대상이 아니며 STM32CubeIDE 또는 각 펌웨어용 ARM 빌드 환경에서 별도로 빌드합니다.
