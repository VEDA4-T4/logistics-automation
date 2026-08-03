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
KillSignal=SIGTERM
TimeoutStopSec=20
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

## RTSP 릴레이 unit

`setup-rtsp-relay.sh`는 저장소의 `logistics-rtsp-relay.service`를 설치하고 활성화합니다. 이 서비스는
`/usr/local/bin/mediamtx`를 `/etc/logistics/rtsp-relay.yml` 설정으로 실행합니다.

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

RTSP 릴레이의 운영 상태, 로그와 네 채널 준비 상태는 중앙 Raspberry Pi에서 다음 순서로 확인합니다.

```sh
sudo systemctl status logistics-rtsp-relay --no-pager
sudo journalctl -u logistics-rtsp-relay -f
./deploy/scripts/check-rtsp-relay.sh
```

unit 또는 INI를 변경한 후에는 해당 서비스만 재시작합니다.

```sh
sudo systemctl restart logistics-central-server
```

`logistics-central-server`를 재시작해도 `logistics-rtsp-relay`를 재시작해서는 안 됩니다. 두 서비스는 독립적으로
운영합니다. 카메라 소스를 변경할 때만 여섯 RTSP 환경 변수를 다시 지정하고 `LOGISTICS_FORCE_CONFIG=1`로
`setup-rtsp-relay.sh`를 실행한 뒤 다음 명령으로 릴레이를 재시작합니다.

```sh
sudo systemctl restart logistics-rtsp-relay
```

비밀번호, 토큰과 개인키를 unit의 `Environment=`에 직접 넣지 말고 권한이 제한된 `/etc/logistics/*.ini` 또는 별도
credentials 파일을 사용합니다.

저장소의 unit 파일은 다음과 같이 설치합니다.

```sh
sudo install -m 0644 deploy/systemd/logistics-central-server.service \
  /etc/systemd/system/logistics-central-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now logistics-central-server
```

중앙 서버는 `SIGTERM`을 받으면 MQTT와 HTTP 수신을 중지하고 공정 상태 저장 및 SQLite WAL
체크포인트를 수행합니다. 재시작 후 진행 중이던 공정은 장비를 자동 구동하지 않고 `STOPPED`
상태로 복원되므로 현장 상태를 확인한 뒤 시작해야 합니다.
