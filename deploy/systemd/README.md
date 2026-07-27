# systemd 운영

중앙서버와 Device 노드를 부팅 시 자동 시작하고 장애 시 재시작하기 위한 기본 예시입니다. 실행 사용자, 저장소 경로와
설정 경로는 배포 환경에 맞게 변경합니다.

## 중앙서버 unit

`/etc/systemd/system/logistics-central-server.service`:

```ini
[Unit]
Description=Logistics Central Server
After=network-online.target mosquitto.service
Wants=network-online.target
Requires=mosquitto.service

[Service]
Type=simple
User=logistics
Group=logistics
WorkingDirectory=/opt/logistics-automation
ExecStart=/opt/logistics-automation/build-central/central-server-rpi/logistics_central_server --config /etc/logistics/server.ini
Restart=on-failure
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

## Vision unit

`/etc/systemd/system/logistics-vision.service`:

```ini
[Unit]
Description=Logistics Vision Node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=logistics
Group=video
WorkingDirectory=/opt/logistics-automation
ExecStart=/opt/logistics-automation/build-vision/device-rpi/logistics_vision_node --headless --config /etc/logistics/vision-node.ini
Restart=on-failure
RestartSec=3
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

카메라와 UART 장치 접근을 위해 배포 사용자에게 필요한 그룹만 부여합니다. 무조건 root로 실행하지 않습니다.

## 적용

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now logistics-central-server
sudo systemctl enable --now logistics-vision
```

## 상태와 로그

```sh
systemctl status logistics-central-server --no-pager
systemctl status logistics-vision --no-pager
journalctl -u logistics-central-server -f
journalctl -u logistics-vision -f
```

unit 또는 INI를 변경한 후에는 해당 서비스만 재시작합니다.

```sh
sudo systemctl restart logistics-central-server
```

비밀번호, 토큰과 개인키를 unit의 `Environment=`에 직접 넣지 말고 권한이 제한된 `/etc/logistics/*.ini` 또는 별도
credentials 파일을 사용합니다.
