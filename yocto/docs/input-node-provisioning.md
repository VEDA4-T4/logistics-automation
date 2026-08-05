# Input Node Yocto 이미지 프로비저닝

## 1. 목적

이 문서는 Raspberry Pi 4용 Input Node 제품 이미지를 빌드하고, MQTT TLS 인증정보와 장치 설정을 설치한 뒤 서비스를 활성화하는 절차를 설명한다.

제품 이미지는 다음 보안 정책을 적용한다.

- `debug-tweaks` 미사용
- root 비밀번호 잠금
- Dropbear 비밀번호 로그인 차단
- 지정된 SSH 공개키만 root 로그인 허용
- MQTT 비밀번호를 이미지와 Git에 저장하지 않음
- 프로비저닝 완료 전 Input Node 서비스 비활성화

## 2. 이미지 빌드

WSL에서 실행한다.

```bash
cd ~/workspace/logistics-automation
```

```bash
export KAS_WORK_DIR="$HOME/yocto-work/logistics"
export KAS_BUILD_DIR="$HOME/yocto-work/logistics/build-input"
```

```bash
kas build yocto/kas/input.yml
```

빌드 결과는 다음 경로에 생성된다.

```text
$KAS_BUILD_DIR/tmp/deploy/images/raspberrypi4-64/
```

`downloads/`와 `sstate-cache/`는 `KAS_WORK_DIR` 아래에서 다른 이미지(sorting 등)와 공유되므로, 다른 이미지를 먼저 빌드했다면 이번 빌드가 더 빠르게 끝난다.

## 3. 이미지 압축 해제 및 Windows 복사

```bash
DEPLOY_DIR="$KAS_BUILD_DIR/tmp/deploy/images/raspberrypi4-64"
```

```bash
IMAGE_BZ2=$(readlink -f "$DEPLOY_DIR/logistics-input-image-raspberrypi4-64.rootfs.wic.bz2")
```

```bash
PRODUCT_IMAGE="/tmp/logistics-input-production.wic"
```

```bash
bzip2 -dc "$IMAGE_BZ2" >"$PRODUCT_IMAGE"
```

```bash
WINDOWS_IMAGE="/mnt/c/Users/사용자명/Downloads/logistics-input-production.img"
```

```bash
cp "$PRODUCT_IMAGE" "$WINDOWS_IMAGE"
```

복사 전후 SHA-256 값이 같은지 확인한다.

```bash
sha256sum "$PRODUCT_IMAGE" "$WINDOWS_IMAGE"
```

## 4. SD카드 기록

Raspberry Pi Imager에서 다음 순서로 진행한다.

1. `Use custom` 선택
2. `logistics-input-production.img` 선택
3. Input Pi용 SD카드 선택
4. 사용자·비밀번호·Wi-Fi 사용자 지정은 적용하지 않음
5. 이미지 기록
6. SD카드를 Raspberry Pi에 장착하고 부팅

## 5. 제품 SSH 접속

제품용 SSH 개인키는 Git에 저장하지 않는다.

기존 장치에 이미지를 다시 기록했다면 이전 호스트 키를 제거한다.

```bash
ssh-keygen -R 장치_IP
```

제품용 키로 접속한다.

```bash
ssh -o IdentitiesOnly=yes \
  -i ~/.ssh/logistics_yocto_admin \
  root@장치_IP
```

정상 hostname은 다음과 같다.

```text
input-node-01
```

확인 명령:

```bash
hostname
```

## 6. CA 인증서 준비

WSL에서 중앙 서버의 CA 인증서를 복사한다.

```bash
scp server@서버_IP:/etc/logistics/tls/ca.crt \
  /tmp/veda-mqtt-test-ca.crt
```

WSL에서 인증서 지문을 확인한다.

```bash
openssl x509 \
  -in /tmp/veda-mqtt-test-ca.crt \
  -noout \
  -fingerprint \
  -sha256
```

제품 SSH 키를 사용해 Input Pi로 전달한다.

```bash
scp -o IdentitiesOnly=yes \
  -i ~/.ssh/logistics_yocto_admin \
  /tmp/veda-mqtt-test-ca.crt \
  root@장치_IP:/tmp/veda-mqtt-test-ca.crt
```

## 7. CA 인증서 설치

Input Pi에서 실행한다.

```bash
mkdir -p /etc/logistics/tls
```

```bash
cp /tmp/veda-mqtt-test-ca.crt /etc/logistics/tls/ca.crt
```

```bash
chown root:root /etc/logistics/tls/ca.crt
```

```bash
chmod 0644 /etc/logistics/tls/ca.crt
```

```bash
rm /tmp/veda-mqtt-test-ca.crt
```

설치 결과와 지문을 확인한다.

```bash
ls -l /etc/logistics/tls/ca.crt
```

```bash
openssl x509 \
  -in /etc/logistics/tls/ca.crt \
  -noout \
  -fingerprint \
  -sha256
```

중앙 서버 원본 CA와 Input Pi에 설치한 CA의 지문이 같아야 한다.

## 8. Input Node 설정

Input Pi에서 예제 설정을 복사한다.

```bash
cp /etc/logistics/input-node.ini.example \
  /etc/logistics/input-node.ini
```

설정을 편집한다.

```bash
vi /etc/logistics/input-node.ini
```

장치 환경에 맞게 다음 항목을 설정한다.

```ini
[device]
device_id=PI-INPUT-01
node_name=input-node-01
ip_address=장치_IP

[mqtt]
host=서버_IP
port=8883
client_id=PI-INPUT-01
username=PI-INPUT-01
password=장치별_MQTT_비밀번호
tls_enabled=true
ca_certificate=/etc/logistics/tls/ca.crt
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=true

[log_upload]
enabled=false
```

`host`는 서버 인증서의 `subjectAltName`에 있는 값과 일치해야 한다. IP와 DNS 이름 중 인증서에 실제로 포함된 쪽을 사용한다. 불일치하면 TLS 핸드셰이크의 hostname 검증 단계에서 실패한다.

실제 MQTT 비밀번호가 들어 있는 설정 파일은 Git에 추가하지 않는다.

서비스 계정이 설정을 읽을 수 있도록 권한을 적용한다.

```bash
chown root:logistics /etc/logistics/input-node.ini
```

```bash
chmod 0640 /etc/logistics/input-node.ini
```

비밀번호를 제외한 설정을 확인한다.

```bash
grep -E \
  '^(device_id|ip_address|host|port|client_id|username|tls_enabled|ca_certificate)=' \
  /etc/logistics/input-node.ini
```

비밀번호가 설정됐는지만 확인한다.

```bash
awk -F= \
  '/^password=/{if(length($2)>0)print "password: SET";else print "password: EMPTY"}' \
  /etc/logistics/input-node.ini
```

## 9. MQTT TLS 검증

Input Pi에서 실행한다.

```bash
openssl s_client \
  -connect 서버_IP:8883 \
  -CAfile /etc/logistics/tls/ca.crt \
  -verify_ip 서버_IP \
  -brief \
  </dev/null
```

다음 결과가 출력돼야 한다.

```text
Verification: OK
```

인증서 SAN이 IP가 아니라 DNS 이름만 포함하는 경우, `-verify_ip` 대신 `-verify_hostname`을 사용하고 `-connect`에도 같은 DNS 이름을 지정한다.

## 10. Input Node 서비스 활성화

프로비저닝이 완료된 후에만 서비스를 활성화한다.

```bash
systemctl enable --now logistics-input-node
```

서비스 상태를 확인한다.

```bash
systemctl status logistics-input-node --no-pager -l
```

최근 로그를 확인한다.

```bash
journalctl -u logistics-input-node -n 30 --no-pager
```

정상 로그:

```text
MQTT broker connected
command topics subscribed
connected: /dev/vedauart
online status and registration published
```

## 11. MQTT 상태 검증

중앙 서버에서 실행한다.

```bash
read -rsp "central-server MQTT password: " MQTT_PASSWORD
echo
```

```bash
mosquitto_sub \
  -h 서버_IP \
  -p 8883 \
  --cafile /etc/logistics/tls/ca.crt \
  -u central-server \
  -P "$MQTT_PASSWORD" \
  -t 'device/PI-INPUT-01/status' \
  -t 'device/PI-INPUT-01/heartbeat' \
  -t 'device/PI-INPUT-01/event' \
  -t 'device/PI-INPUT-01/response' \
  -t 'device/PI-INPUT-01/error' \
  -v
```

정상 기준:

- `DEVICE_STATUS`의 상태가 `ONLINE`
- heartbeat가 설정된 `keep_alive_seconds` 주기로 수신
- 예상하지 않은 `ERROR_OCCURRED`가 지속 발생하지 않음

`central-server` client_id는 실제 중앙서버 프로세스와 충돌하므로, 진단용 구독에는 `-i` 옵션으로 별도 client_id를 지정한다.

## 12. 재부팅 자동 복구 검증

Input Pi에서 실행한다.

```bash
reboot
```

재접속 후 확인한다.

```bash
systemctl status logistics-input-node --no-pager -l
```

재부팅 후에도 다음 항목이 자동 복구돼야 한다.

- Input Node systemd 서비스
- MQTT TLS 연결
- MQTT 명령 토픽 구독
- `/dev/vedauart` 연결
- ONLINE 상태와 장치 등록 발행
- heartbeat 발행

## 13. 보안 확인

```bash
cat /etc/default/dropbear
```

정상 결과:

```text
DROPBEAR_EXTRA_ARGS="-s"
```

SSH 키 권한 확인:

```bash
stat -c '%a %U:%G %n' \
  /root/.ssh \
  /root/.ssh/authorized_keys
```

정상 결과:

```text
700 root:root /root/.ssh
600 root:root /root/.ssh/authorized_keys
```

root 비밀번호 잠금 확인:

```bash
awk -F: \
  '$1=="root"{if($2=="")print "root password: EMPTY";else if($2~/^[!*]/)print "root password: LOCKED";else print "root password: HASHED"}' \
  /etc/shadow
```

정상 결과:

```text
root password: LOCKED
```

## 14. 인증정보 관리 정책

- SSH 개인키는 Git에 저장하지 않는다.
- MQTT 비밀번호는 이미지와 Git에 저장하지 않는다.
- 실제 `input-node.ini`는 Git에 저장하지 않는다.
- SSH 공개키 교체 시 `authorized_keys`를 변경하고 이미지를 다시 빌드한다.
- CA 교체 시 중앙 서버 원본과 장치 인증서의 SHA-256 지문을 비교한다.
- SSH 개인키를 분실하면 새 공개키를 포함한 이미지를 다시 빌드한다.
- 새로운 SD카드에서는 프로비저닝 완료 후 `systemctl enable --now`를 한 번 실행한다.

## 15. 알려진 제약

- `input-node.ini.example`은 `device-rpi/config/`가 아니라 recipe의 `files/`에서 온다. recipe가 고정한 `SRCREV`(`7a579c5`) 시점에는 저장소에 이 예제 파일이 없었기 때문이다. `SRCREV`가 그 이후 커밋으로 갱신되면 `logistics-input-node.bb`의 `do_install`을 `${S}/device-rpi/config/input-node.ini.example` 참조로 바꾸고 recipe 내부 사본은 제거한다.
