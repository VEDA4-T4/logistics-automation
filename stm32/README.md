# STM32 firmware

실시간 센서 입력, 모터/서보 제어, 상태 머신, 오류 감지와 비상정지를 담당합니다. 각 CubeMX 프로젝트는
`Core/`, `Drivers/`, `Application/`을 유지하고 공통 UART 계약과 안전 규칙은 `common/`에서 공유합니다.

호스트 CMake 빌드에는 포함하지 않으며 STM32CubeIDE 또는 별도의 ARM toolchain으로 빌드합니다.

