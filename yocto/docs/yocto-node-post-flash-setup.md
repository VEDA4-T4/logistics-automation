아래 문서는 특정 노드에 종속되지 않은 **SD 카드 플래시 이후 공통 가이드**입니다. Input과 Sorting은 현재 확정된 값을 제공하고, Vision과 Linetracer는 레시피가 완성되면 노드 정보 표에 추가하면 됩니다.

저장할 파일:

```bash
vim yocto/docs/yocto-node-post-flash-setup.md
```

---

# Yocto Logistics 노드 SD 카드 플래시 후 공통 설정 가이드

## 1. 목적

이 문서는 Yocto 이미지를 SD 카드에 기록한 이후 다음 작업을 수행하는 공통 절차를 설명한다.

```text
SD 카드 이미지 기록
    ↓
Wi-Fi 설정
    ↓
최초 부팅
    ↓
SSH 접속
    ↓
Wi-Fi 검증
    ↓
MQTT CA 인증서 설치
    ↓
MQTT TLS 검증
    ↓
노드별 INI 설정
    ↓
노드 서비스 활성화
    ↓
재부팅 검증
    ↓
중앙 서버 MQTT 메시지 확인
```

실제 Wi-Fi 비밀번호와 MQTT 비밀번호는 Git이나 문서에 기록하지 않는다.

---

## 2. 노드별 설정값

작업을 시작하기 전에 대상 노드의 설정값을 확인한다.

| 항목 | Input Node | Sorting Node |
|---|---|---|
| 이미지 | `logistics-input-image` | `logistics-sorting-image` |
| 호스트명 | `input-node-01` | `sorting-node-01` |
| mDNS 주소 | `input-node-01.local` | `sorting-node-01.local` |
| 장치 ID | `PI-INPUT-01` | `PI-SORTING-01` |
| INI 예제 | `input-node.ini.example` | `sorting-node.ini.example` |
| 실제 INI | `input-node.ini` | `sorting-node.ini` |
| 서비스 | `logistics-input-node.service` | `logistics-sorting-node.service` |
| 실행 파일 | `logistics_input_node` | `logistics_sorting_node` |
| UART 장치 | `/dev/vedauart` | `/dev/vedauart` |

현재 MQTT 서버 정보:

```text
MQTT 서버 IP:  172.20.33.72
MQTT TLS 포트: 8883
```

Vision과 Linetracer는 이미지 및 레시피 구현이 완료되면 이 표에 추가한다.

---

## 3. SD 카드 이미지 기록

Windows에서 Raspberry Pi Imager 등의 프로그램을 사용하여 대상 노드 이미지를 SD 카드에 기록한다.

Input 이미지 예시:

```text
logistics-input-production.img
```

Sorting 이미지 예시:

```text
logistics-sorting-production.img
```

이미지 기록이 끝나면 SD 카드를 바로 제거하지 않고 Windows에서 `boot` 파티션이 다시 나타나는지 확인한다.

예시:

```text
boot (D:)
```

---

## 4. Wi-Fi 설정 파일 생성

### 4.1 Wi-Fi 예제 파일 복사

Windows PowerShell에서 실행한다.

Boot 파티션이 `D:`가 아니라면 실제 드라이브 문자로 변경한다.

```powershell
Copy-Item D:\logistics-wifi.conf.example D:\logistics-wifi.conf
```

파일 이름은 정확히 다음과 같아야 한다.

```text
logistics-wifi.conf
```

다음과 같은 파일 이름은 인식되지 않는다.

```text
logistics-wifi.conf.txt
logistics-wifi.conf.example
```

### 4.2 Wi-Fi 설정 편집

```powershell
notepad D:\logistics-wifi.conf
```

설정 예시:

```ini
ctrl_interface=/run/wpa_supplicant
update_config=0
country=KR

sae_pwe=2

network={
    ssid="GSC_GangNam"
    psk="실제_Wi-Fi_비밀번호"
    key_mgmt=WPA-PSK SAE
    ieee80211w=1
    scan_ssid=1
}
```

확인 사항:

```text
[ ] 실제 SSID 입력
[ ] 실제 Wi-Fi 비밀번호 입력
[ ] 따옴표 유지
[ ] REPLACE_WITH_ 문자열 제거
[ ] 파일 확장자가 .txt가 아닌지 확인
```

설정 완료 후 파일을 저장하고 SD 카드를 안전하게 제거한다.

---

## 5. Raspberry Pi 최초 부팅

SD 카드를 대상 Raspberry Pi에 넣고 전원을 켠다.

Wi-Fi만 테스트하려면 유선 랜선을 연결하지 않고 부팅한다.

최초 부팅 시 다음 작업이 자동으로 진행된다.

```text
/boot/logistics-wifi.conf 발견
    ↓
파일 크기 및 placeholder 검사
    ↓
SSID 및 PSK/SAE 설정 검사
    ↓
Windows CRLF 줄바꿈 제거
    ↓
/etc/wpa_supplicant/wpa_supplicant-wlan0.conf 생성
    ↓
root:root, 0600 권한 적용
    ↓
wpa_supplicant@wlan0.service 활성화
    ↓
Wi-Fi 연결
    ↓
DHCP 주소 할당
    ↓
Boot 파티션의 평문 logistics-wifi.conf 삭제
    ↓
Avahi mDNS 호스트명 광고
```

최초 부팅과 Wi-Fi 연결에는 시간이 걸릴 수 있다.

---

## 6. Windows에서 노드 검색

### Input Node

```powershell
ping -4 input-node-01.local
```

### Sorting Node

```powershell
ping -4 sorting-node-01.local
```

정상 예시:

```text
Ping sorting-node-01.local [172.20.27.56]
```

출력된 IPv4 주소를 기록한다. DHCP 환경에서는 재부팅이나 네트워크 변경 후 주소가 달라질 수 있다.

`.local` 이름이 검색되지 않으면 다음을 확인한다.

```text
1. Raspberry Pi 부팅 완료 여부
2. Wi-Fi 설정 파일의 SSID와 비밀번호
3. PC와 Raspberry Pi가 같은 네트워크에 있는지
4. 잠시 기다린 후 ping 재시도
5. 필요하면 유선 랜선으로 임시 접속
```

---

## 7. WSL에서 작업 변수 설정

대상 노드에 맞게 설정한다.

### Input Node

```bash
NODE_HOST="input-node-01.local"
DEVICE_ID="PI-INPUT-01"
INI_NAME="input-node.ini"
SERVICE="logistics-input-node.service"
```

### Sorting Node

```bash
NODE_HOST="sorting-node-01.local"
DEVICE_ID="PI-SORTING-01"
INI_NAME="sorting-node.ini"
SERVICE="logistics-sorting-node.service"
```

공통 변수를 설정한다.

```bash
SERVER_IP="172.20.33.72"
SSH_KEY="$HOME/.ssh/logistics_yocto_admin"
CA_TMP="/tmp/veda-mqtt-ca.crt"
```

이 변수들은 현재 WSL 셸에서만 유지된다. 새 터미널을 열면 다시 설정한다.

---

## 8. SSH 접속

WSL에서 실행한다.

```bash
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST"
```

mDNS 접속이 안 되면 Windows에서 확인한 IP를 사용한다.

```bash
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" root@노드_IP
```

이미지를 다시 구운 후 호스트 키 충돌이 발생하면 기존 키를 삭제한다.

```bash
ssh-keygen -R "$NODE_HOST"
```

IP로 접속했다면 IP의 기존 키도 삭제한다.

```bash
ssh-keygen -R 노드_IP
```

그다음 다시 접속한다.

---

## 9. Wi-Fi 연결 검증

다음 명령은 Raspberry Pi에서 실행한다.

### 9.1 IP 주소 확인

```bash
ip -4 address show wlan0
```

정상 예시:

```text
inet 172.20.27.56/23
```

### 9.2 연결된 Wi-Fi 확인

```bash
iw dev wlan0 link
```

정상 출력에 다음 내용이 포함된다.

```text
Connected to ...
SSID: GSC_GangNam
signal: ...
rx bitrate: ...
tx bitrate: ...
```

### 9.3 Wi-Fi 커널 모듈 확인

```bash
lsmod | grep -E 'brcmfmac_wcc|brcmfmac|brcmutil|cfg80211'
```

정상적으로 다음 모듈이 표시되어야 한다.

```text
brcmfmac_wcc
brcmfmac
brcmutil
cfg80211
```

### 9.4 관련 서비스 확인

```bash
systemctl is-active logistics-wifi-provision.service
systemctl is-active wpa_supplicant@wlan0.service
systemctl is-active avahi-daemon.service
```

정상 결과:

```text
active
active
active
```

프로비저닝 상세 로그:

```bash
systemctl status logistics-wifi-provision.service --no-pager -l
```

정상 로그:

```text
Wi-Fi configuration imported successfully
```

### 9.5 Wi-Fi 설정 권한 확인

```bash
stat -c '%a %U:%G %n' /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
```

정상 결과:

```text
600 root:root /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
```

Boot 파티션의 평문 비밀번호 파일이 삭제됐는지 확인한다.

```bash
test ! -e /boot/logistics-wifi.conf && echo "boot credential removed"
```

정상 결과:

```text
boot credential removed
```

---

## 10. MQTT 서버 통신 확인

Raspberry Pi에서 실행한다.

```bash
ping -c 2 172.20.33.72
```

응답이 없으면 다음을 확인한다.

```text
[ ] MQTT 서버 전원
[ ] MQTT 서버 IP
[ ] Wi-Fi와 서버 네트워크 사이의 라우팅
[ ] 서버 방화벽
[ ] Raspberry Pi 기본 게이트웨이
```

---

## 11. MQTT CA 인증서 가져오기

Raspberry Pi에서 로그아웃하고 WSL에서 실행한다.

### 11.1 서버에서 CA 인증서 복사

```bash
scp server@172.20.33.72:/etc/logistics/tls/ca.crt /tmp/veda-mqtt-ca.crt
```

### 11.2 서버 원본 지문 확인

```bash
ssh server@172.20.33.72 "openssl x509 -in /etc/logistics/tls/ca.crt -noout -fingerprint -sha256"
```

### 11.3 복사한 인증서 지문 확인

```bash
openssl x509 -in /tmp/veda-mqtt-ca.crt -noout -fingerprint -sha256
```

두 지문이 정확히 같아야 한다.

현재 확인된 지문:

```text
01:4A:DE:B1:72:66:25:69:DD:49:1C:4E:30:B0:A5:43:C2:0E:DB:F8:6C:E6:67:AD:35:B0:B2:02:76:BC:D2:82
```

지문이 다르면 인증서를 설치하지 않는다.

---

## 12. CA 인증서를 노드로 복사

WSL에서 설정한 `$NODE_HOST`와 `$SSH_KEY`를 사용한다.

```bash
scp -o IdentitiesOnly=yes -i "$SSH_KEY" "$CA_TMP" "root@$NODE_HOST:/tmp/veda-mqtt-ca.crt"
```

mDNS가 동작하지 않으면 IP를 사용한다.

```bash
scp -o IdentitiesOnly=yes -i "$SSH_KEY" "$CA_TMP" root@노드_IP:/tmp/veda-mqtt-ca.crt
```

---

## 13. 노드에서 CA 인증서 설치

노드에 다시 SSH로 접속한다.

```bash
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST"
```

노드에서 복사된 인증서 지문을 확인한다.

```bash
openssl x509 -in /tmp/veda-mqtt-ca.crt -noout -fingerprint -sha256
```

서버 원본과 지문이 같으면 설치한다.

```bash
mkdir -p /etc/logistics/tls
cp /tmp/veda-mqtt-ca.crt /etc/logistics/tls/ca.crt
chown root:root /etc/logistics/tls/ca.crt
chmod 0644 /etc/logistics/tls/ca.crt
rm /tmp/veda-mqtt-ca.crt
```

설치 결과를 확인한다.

```bash
ls -l /etc/logistics/tls/ca.crt
openssl x509 -in /etc/logistics/tls/ca.crt -noout -fingerprint -sha256
```

정상 권한:

```text
-rw-r--r-- root root /etc/logistics/tls/ca.crt
```

---

## 14. MQTT TLS 인증서 검증

노드에서 실행한다.

```bash
openssl s_client -connect 172.20.33.72:8883 -CAfile /etc/logistics/tls/ca.crt -verify_ip 172.20.33.72 -brief </dev/null
```

정상 결과:

```text
CONNECTION ESTABLISHED
Protocol version: TLSv1.3
Verification: OK
```

다음 메시지는 IP 접속에서 발생할 수 있으며 오류가 아니다.

```text
Can't use SSL_get_servername
```

판정 기준은 다음 항목이다.

```text
Verification: OK
```

---

## 15. 노드별 실제 INI 생성

노드 종류에 따라 다음 변수를 노드 셸에서 설정한다.

### Input Node

```bash
INI_NAME="input-node.ini"
SERVICE="logistics-input-node.service"
DEVICE_ID="PI-INPUT-01"
```

### Sorting Node

```bash
INI_NAME="sorting-node.ini"
SERVICE="logistics-sorting-node.service"
DEVICE_ID="PI-SORTING-01"
```

예제 파일을 실제 설정으로 복사한다.

```bash
cp "/etc/logistics/${INI_NAME}.example" "/etc/logistics/${INI_NAME}"
```

현재 Wi-Fi IP를 확인한다.

```bash
ip -4 address show wlan0
```

설정 파일을 연다.

```bash
vi "/etc/logistics/${INI_NAME}"
```

공통적으로 `[device]`와 `[mqtt]` 항목을 수정한다.

```ini
[device]
device_id=노드별_DEVICE_ID
node_name=노드별_호스트명
ip_address=노드의_현재_Wi-Fi_IP

[mqtt]
host=172.20.33.72
port=8883
client_id=노드별_DEVICE_ID
username=노드별_DEVICE_ID
password=해당_노드의_MQTT_비밀번호
tls_enabled=true
ca_certificate=/etc/logistics/tls/ca.crt
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=false
```

노드별 나머지 설정은 예제 파일 값을 유지하거나 실제 장치 환경에 맞게 수정한다.

Input Node 예시:

```ini
[log_upload]
enabled=false

[image_upload]
enabled=false
```

Sorting Node 예시:

```ini
[sorting]
default_speed=50
```

주의사항:

- 각 노드는 서로 다른 `device_id`, `client_id`, `username`, 비밀번호를 사용한다.
- 실제 비밀번호를 Git, 문서, 채팅에 기록하지 않는다.
- DHCP로 IP가 변경되면 `ip_address`를 변경한다.
- MQTT 서버 IP가 변경되면 `host`도 변경한다.

---

## 16. INI 파일 권한 설정

노드에서 실행한다.

```bash
chown root:logistics "/etc/logistics/${INI_NAME}"
chmod 0640 "/etc/logistics/${INI_NAME}"
```

권한을 확인한다.

```bash
stat -c '%a %U:%G %n' "/etc/logistics/${INI_NAME}"
```

정상 결과:

```text
640 root:logistics
```

비밀번호를 출력하지 않고 설정 여부만 확인한다.

```bash
awk -F= '/^password=/{if(length($2)>0)print "password: SET";else print "password: EMPTY"}' "/etc/logistics/${INI_NAME}"
```

정상 결과:

```text
password: SET
```

공개 가능한 설정만 확인한다.

```bash
grep -E '^(device_id|node_name|ip_address|host|port|client_id|username|tls_enabled|ca_certificate)=' "/etc/logistics/${INI_NAME}"
```

---

## 17. VEDAUART 확인

이 단계는 `/dev/vedauart`를 사용하는 노드에서만 수행한다.

현재 Input과 Sorting Node가 해당한다.

장치 파일 확인:

```bash
ls -l /dev/vedauart
```

정상 예시:

```text
crw-rw---- root dialout /dev/vedauart
```

모듈 확인:

```bash
lsmod | grep vedauart
```

커널 로그 확인:

```bash
dmesg | grep -i vedauart
```

정상 로그 예시:

```text
vedauart ready at 115200 baud
```

Vision 또는 Linetracer가 VEDAUART를 사용하지 않는다면 이 단계는 생략한다.

---

## 18. 노드 서비스 활성화

노드에서 설정한 `$SERVICE`를 사용한다.

```bash
systemctl enable --now "$SERVICE"
```

서비스 상태를 확인한다.

```bash
systemctl status "$SERVICE" --no-pager -l
```

정상 상태:

```text
Active: active (running)
```

정상 로그 예시:

```text
MQTT connection started
daemon started
UART connected
MQTT broker connected
command topics subscribed
online status and registration published
```

최근 로그 확인:

```bash
journalctl -u "$SERVICE" -n 50 --no-pager
```

실시간 로그 확인:

```bash
journalctl -fu "$SERVICE"
```

실시간 로그 종료:

```text
Ctrl+C
```

---

## 19. 재부팅 후 자동 시작 검증

노드에서 실행한다.

```bash
reboot
```

부팅이 완료되면 WSL에서 다시 접속한다.

```bash
ssh -o IdentitiesOnly=yes -i "$SSH_KEY" "root@$NODE_HOST"
```

SSH 재접속 후 노드 종류에 맞게 `$SERVICE`를 다시 설정한다.

Input Node:

```bash
SERVICE="logistics-input-node.service"
```

Sorting Node:

```bash
SERVICE="logistics-sorting-node.service"
```

Wi-Fi 상태 확인:

```bash
systemctl is-active wpa_supplicant@wlan0.service
ip -4 address show wlan0
```

노드 서비스 확인:

```bash
systemctl is-enabled "$SERVICE"
systemctl is-active "$SERVICE"
```

정상 결과:

```text
enabled
active
```

상세 상태 확인:

```bash
systemctl status "$SERVICE" --no-pager -l
```

재부팅 직후에는 Wi-Fi와 `network-online.target`이 아직 준비되지 않아 잠시 `inactive`로 표시될 수 있다. 잠시 기다린 후 다시 확인한다.

```bash
systemctl is-active "$SERVICE"
```

---

## 20. 중앙 서버에서 MQTT 메시지 확인

중앙 서버에서 실행한다.

대상 장치 ID를 설정한다.

Input Node:

```bash
DEVICE_ID="PI-INPUT-01"
```

Sorting Node:

```bash
DEVICE_ID="PI-SORTING-01"
```

중앙 서버 MQTT 비밀번호를 입력한다.

```bash
read -rsp "central-server MQTT password: " MQTT_PASSWORD; echo
```

대상 노드의 모든 MQTT Topic을 확인한다.

```bash
mosquitto_sub -h 172.20.33.72 -p 8883 --cafile /etc/logistics/tls/ca.crt -u central-server -P "$MQTT_PASSWORD" -t "device/${DEVICE_ID}/#" -v
```

확인 가능한 Topic 예시:

```text
device/DEVICE_ID/register
device/DEVICE_ID/status
device/DEVICE_ID/heartbeat
device/DEVICE_ID/event
device/DEVICE_ID/response
device/DEVICE_ID/error
```

정상적으로 다음 정보를 확인해야 한다.

```text
[ ] ONLINE 상태
[ ] registration 메시지
[ ] 주기적인 heartbeat
[ ] 센서 또는 장치 event
[ ] 오류 메시지 없음
```

`Connection Refused: not authorised`가 발생하면 현재 셸에 비밀번호가 설정됐는지 확인하고 다음 명령부터 다시 실행한다.

```bash
read -rsp "central-server MQTT password: " MQTT_PASSWORD; echo
```

---

## 21. 전체 완료 체크리스트

```text
[ ] 올바른 노드 이미지 SD 카드 기록
[ ] Bootfs에 logistics-wifi.conf 생성
[ ] Wi-Fi placeholder 제거
[ ] wlan0 IPv4 주소 할당
[ ] Wi-Fi AP 연결 확인
[ ] brcmfmac_wcc 모듈 확인
[ ] wpa_supplicant 서비스 active
[ ] avahi-daemon 서비스 active
[ ] 노드이름.local 검색 가능
[ ] Bootfs 평문 Wi-Fi 설정 삭제
[ ] MQTT 서버 ping 성공
[ ] 서버와 복사본의 CA 지문 일치
[ ] 노드 CA 인증서 설치
[ ] TLS Verification: OK
[ ] 노드별 실제 INI 생성
[ ] INI 권한 0640 root:logistics
[ ] MQTT 비밀번호 SET 확인
[ ] 필요한 경우 /dev/vedauart 확인
[ ] 노드 서비스 enabled
[ ] 노드 서비스 active
[ ] MQTT broker connected 로그 확인
[ ] 재부팅 후 Wi-Fi 자동 연결
[ ] 재부팅 후 노드 서비스 자동 실행
[ ] 중앙 서버에서 status 확인
[ ] 중앙 서버에서 heartbeat 확인
[ ] 중앙 서버에서 event 확인
```

---

## 22. 변경 상황별 대응

| 변경 사항 | 이미지 재빌드 | 대응 |
|---|---:|---|
| 노드 Wi-Fi IP 변경 | 불필요 | INI의 `ip_address` 수정 |
| Wi-Fi SSID 변경 | 불필요 | 새 `logistics-wifi.conf` 배치 |
| Wi-Fi 비밀번호 변경 | 불필요 | 새 `logistics-wifi.conf` 배치 |
| MQTT 계정 비밀번호 변경 | 불필요 | 노드 INI 수정 |
| MQTT 서버 IP 변경 | 불필요 | 서버 인증서 재발급 및 INI `host` 수정 |
| MQTT CA 변경 | 불필요 | 모든 노드에 새 `ca.crt` 배포 |
| 노드 애플리케이션 변경 | 필요 | `SRCREV` 갱신 및 이미지 재빌드 |
| 커널 드라이버 변경 | 필요 | 드라이버와 이미지 재빌드 |
| SSH 공개키 추가 | 기존 장치는 수동 추가 가능 | 향후 이미지에는 재빌드 필요 |

설정 변경 후 노드 서비스만 다시 시작하려면 실행한다.

```bash
systemctl restart "$SERVICE"
systemctl status "$SERVICE" --no-pager -l
```

---
