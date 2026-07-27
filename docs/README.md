# 문서 허브

루트 [README](../README.md)는 프로젝트 개요와 빠른 시작만 제공합니다. 작업 목적에 맞는 상세 문서는 아래에서
선택합니다.

## 설치

- [Control Center](../control-center/README.md)
- [Central Server](../central-server-rpi/README.md)
- [Device Raspberry Pi Nodes](../device-rpi/README.md)
- [Ubuntu/Raspberry Pi 설치](setup/ubuntu-rpi.md)
- [Windows Qt 관제 시스템 설치](setup/windows-control-center.md)
- [배포 스크립트 사용법](../deploy/scripts/README.md)
- [VEDAUART 커널 드라이버](../device-rpi/kernel/vedauart/README.md)

## 개발

- [프로젝트 구조와 의존성 규칙](architecture/project-structure.md)
- [빌드 및 테스트](guides/compilation-and-tests.md)
- [런타임 설정](guides/runtime-configuration.md)
- [통신 계약 안내](guides/communication-contracts.md)
- [MQTT 계약](../shared/contracts/mqtt/README.md)
- [HTTP(S) 계약](../shared/contracts/http/README.md)
- [UART 계약](../shared/contracts/uart/README.md)
- [계약 버전 관리](../shared/contracts/VERSIONING.md)

## 실행과 운영

- [통합 실행 가이드](guides/integration-runbook.md)
- [운영 점검과 문제 해결](guides/operations-troubleshooting.md)
- [Mosquitto 보안 및 TLS](../deploy/mosquitto/README.md)
- [systemd 운영](../deploy/systemd/README.md)
- [Vision 노드](../device-rpi/vision-node/README.md)
- [HTTP 업로드 서버](../central-server-rpi/src/http_upload/README.md)
- [데이터베이스 마이그레이션](../central-server-rpi/db/migrations/README.md)

## 문서 작성 원칙

- 현재 코드에서 확인되는 옵션과 명령만 “지원”으로 표기합니다.
- 예정 기능은 “미구현” 또는 “마이그레이션 계획”으로 구분합니다.
- 비밀값 대신 환경 변수나 예시 문자열을 사용합니다.
- 로컬 설정은 `.ini.example`을 복사해 `runtime/` 아래에 만들고 Git에 포함하지 않습니다.
