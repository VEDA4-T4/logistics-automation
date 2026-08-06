# Deployment

배포 자동화와 운영 설정을 구성 요소별로 관리합니다.

- [설치 스크립트](scripts/README.md)
- [RTSP 릴레이 설치](scripts/README.md#rtsp-릴레이)
- [Mosquitto 인증·ACL·TLS](mosquitto/README.md)
- [systemd 서비스](systemd/README.md)

실제 인증서, 개인키, 비밀번호 파일, ACL 운영본과 INI는 이 폴더에 커밋하지 않습니다. 예시는 문서로만 제공하고 실제
파일은 대상 기기의 `/etc/logistics`, `/etc/mosquitto`에서 권한을 제한해 관리합니다.

RTSP 릴레이는 ARM64 중앙 Raspberry Pi에서 `./deploy/scripts/setup-rtsp-relay.sh`로 설치합니다. 실제 카메라 URL과
릴레이 비밀번호는 이 저장소가 아니라 `/etc/logistics/rtsp-relay.yml`과 커밋하지 않는 Qt 런타임 INI에만 둡니다.
v1은 RTSPS를 제공하지 않으므로 Qt와 Raspberry Pi 사이 네트워크는 신뢰할 수 있어야 하며, 그렇지 않으면 VPN으로
보호해야 합니다.
