# Deployment

배포 자동화와 운영 설정을 구성 요소별로 관리합니다.

- [설치 스크립트](scripts/README.md)
- [Mosquitto 인증·ACL·TLS](mosquitto/README.md)
- [systemd 서비스](systemd/README.md)

실제 인증서, 개인키, 비밀번호 파일, ACL 운영본과 INI는 이 폴더에 커밋하지 않습니다. 예시는 문서로만 제공하고 실제
파일은 대상 기기의 `/etc/logistics`, `/etc/mosquitto`에서 권한을 제한해 관리합니다.
