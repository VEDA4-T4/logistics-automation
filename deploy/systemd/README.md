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

승인된 relay reader username은 `control-centor`이고 비밀번호는 별도로 전달합니다. 비밀번호를 percent-encode한 뒤
커밋하지 않는 Qt runtime INI에서 다음 endpoint에만 결합합니다. 아래 URL 자체에는 계정 정보를 넣지 않았습니다.

```text
rtsp://172.20.33.72:8554/channel1
rtsp://172.20.33.72:8554/channel2
rtsp://172.20.33.72:8554/channel3
rtsp://172.20.33.72:8554/channel4
```

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

RTSP 릴레이 setup 직후 중앙 Raspberry Pi에서 enablement, 실행 상태와 네 채널 준비 상태를 순서대로 확인합니다.

```sh
systemctl is-enabled logistics-rtsp-relay.service
systemctl is-active logistics-rtsp-relay.service
./deploy/scripts/check-rtsp-relay.sh
```

각 출력은 `enabled`, `active`, `RTSP relay ready: channel1 channel2 channel3 channel4`여야 합니다. 상세 상태가 필요하면
`sudo systemctl status logistics-rtsp-relay --no-pager`를 실행합니다.

always-on 연결은 Qt를 완전히 종료한 뒤 확인합니다. 첫 readiness 확인 후 30초 동안 reader가 없는 상태를 유지하고 다시
확인합니다. journal은 credential-bearing source URL이 남지 않도록 화면 출력부터 가린 뒤, reader 종료 때문에 upstream
source가 멈춘 기록이 없는지 확인합니다.

```sh
./deploy/scripts/check-rtsp-relay.sh
sleep 30
./deploy/scripts/check-rtsp-relay.sh
sudo journalctl -u logistics-rtsp-relay --since '-1 min' --no-pager \
  | sed -E 's#(rtsps?://)[^/@[:space:]]+(:[^@[:space:]]*)?@#\1[credentials]@#g'
```

production sign-off 전에 Qt runtime INI를 위 네 relay endpoint와 별도 전달된 계정으로 설정하고 채널마다 다음을 확인합니다.

- connecting 이후 H.264 영상이 카메라 설정 해상도로 계속 재생된다.
- ONVIF bounding box가 같은 채널 영상 위에 표시된다.
- `sudo systemctl stop logistics-rtsp-relay` 시 기존 disconnected/error 상태와 주기적 reconnect가 나타나고,
  `sudo systemctl start logistics-rtsp-relay` 후 Qt 재시작 없이 영상과 overlay가 복구된다.
- `logistics-central-server`만 재시작해도 relay 재생은 끊기지 않는다.

마지막으로 대표 채널의 direct URL과 credential-bearing relay URL을 non-echoing prompt로 받아 stream 설명을 비교합니다.
JSON에는 credential을 기록하지 않습니다. 아래 Bash subshell의 EXIT/signal trap은 URL 변수를 제거하며, 기존 interactive
shell의 변수와 trap 상태는 바꾸지 않습니다. `ffprobe` 오류는 credential을 가린 뒤 표시하고 원래 종료 상태는 유지합니다.

```bash
(
  cleanup_ffprobe() {
    unset DIRECT_RTSP_URL RELAY_RTSP_URL
  }
  trap cleanup_ffprobe EXIT
  trap 'cleanup_ffprobe; exit 129' HUP
  trap 'cleanup_ffprobe; exit 130' INT
  trap 'cleanup_ffprobe; exit 143' TERM

  printf 'Direct RTSP URL: '; read -rs DIRECT_RTSP_URL; printf '\n'
  printf 'Relay RTSP URL: '; read -rs RELAY_RTSP_URL; printf '\n'

  ffprobe_status=0
  ffprobe -v error -rtsp_transport tcp -show_streams -of json "${DIRECT_RTSP_URL}" >direct.json \
    2> >(sed -E 's#(rtsps?://)[^/@[:space:]]+(:[^@[:space:]]*)?@#\1[credentials]@#g' >&2) \
    || ffprobe_status=$?
  if ((ffprobe_status == 0)); then
    ffprobe -v error -rtsp_transport tcp -show_streams -of json "${RELAY_RTSP_URL}" >relay.json \
      2> >(sed -E 's#(rtsps?://)[^/@[:space:]]+(:[^@[:space:]]*)?@#\1[credentials]@#g' >&2) \
      || ffprobe_status=$?
  fi
  exit "${ffprobe_status}"
)
```

두 결과의 codec name, width, height, frame rate와 metadata/data track이 같아야 합니다. 같은 camera clock 장면과 Qt 설정으로
direct/relay end-to-end latency도 측정해 추가 지연을 기록합니다.

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
