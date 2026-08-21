# Sorting Node Yocto 이미지 프로비저닝

## 1. 목적

이 문서는 Raspberry Pi 4용 Sorting Node 제품 이미지를 빌드하고, MQTT TLS 인증정보와 장치 설정을 설치한 뒤 서비스를 활성화하는 절차를 설명한다.

제품 이미지는 다음 보안 정책을 적용한다.

- `debug-tweaks` 미사용
- root 비밀번호 잠금
- Dropbear 비밀번호 로그인 차단
- 지정된 SSH 공개키만 root 로그인 허용
- MQTT 비밀번호를 이미지와 Git에 저장하지 않음
- 프로비저닝 완료 전 Sorting Node 서비스 비활성화

## 2. 이미지 빌드

WSL에서 실행한다.

```bash
cd ~/workspace/logistics-automation
```

```bash
export KAS_WORK_DIR="$HOME/yocto-work/logistics"
export KAS_BUILD_DIR="$HOME/yocto-work/logistics/build-sorting"
```

```bash
kas build yocto/kas/sorting.yml
```

빌드 결과는 다음 경로에 생성된다.

```text
$KAS_BUILD_DIR/tmp/deploy/images/raspberrypi4-64/
```

## 3. 이미지 압축 해제 및 Windows 복사

```bash
DEPLOY_DIR="$KAS_BUILD_DIR/tmp/deploy/images/raspberrypi4-64"
```

```bash
IMAGE_BZ2=$(readlink -f "$DEPLOY_DIR/logistics-sorting-image-raspberrypi4-64.rootfs.wic.bz2")
```

```bash
PRODUCT_IMAGE="/tmp/logistics-sorting-production.wic"
```

```bash
bzip2 -dc "$IMAGE_BZ2" >"$PRODUCT_IMAGE"
```

```bash
WINDOWS_IMAGE="/mnt/c/Users/3-20/Downloads/logistics-sorting-production.img"
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
2. `logistics-sorting-production.img` 선택
3. Sorting Pi용 SD카드 선택
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
sorting-node-01
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

제품 SSH 키를 사용해 Sorting Pi로 전달한다.

```bash
scp -o IdentitiesOnly=yes \
  -i ~/.ssh/logistics_yocto_admin \
  /tmp/veda-mqtt-test-ca.crt \
  root@장치_IP:/tmp/veda-mqtt-test-ca.crt
```

## 7. CA 인증서 설치

Sorting Pi에서 실행한다.

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

중앙 서버 원본 CA와 Sorting Pi에 설치한 CA의 지문이 같아야 한다.

## 8. Sorting Node 설정

Sorting Pi에서 예제 설정을 복사한다.

```bash
cp /etc/logistics/sorting-node.ini.example \
  /etc/logistics/sorting-node.ini
```

설정을 편집한다.

```bash
vi /etc/logistics/sorting-node.ini
```

장치 환경에 맞게 다음 항목을 설정한다.

```ini
[device]
device_id=PI-SORTING-01
node_name=sorting-node-01
ip_address=장치_IP

[mqtt]
host=서버_IP
port=8883
client_id=PI-SORTING-01
username=PI-SORTING-01
password=장치별_MQTT_비밀번호
tls_enabled=true
ca_certificate=/etc/logistics/tls/ca.crt
keep_alive_seconds=30
reconnect_min_delay_seconds=1
reconnect_max_delay_seconds=30
clean_session=false

[sorting]
default_speed=50
```

실제 MQTT 비밀번호가 들어 있는 설정 파일은 Git에 추가하지 않는다.

서비스 계정이 설정을 읽을 수 있도록 권한을 적용한다.

```bash
chown root:logistics /etc/logistics/sorting-node.ini
```

```bash
chmod 0640 /etc/logistics/sorting-node.ini
```

비밀번호를 제외한 설정을 확인한다.

```bash
grep -E \
  '^(device_id|ip_address|host|port|client_id|username|tls_enabled|ca_certificate)=' \
  /etc/logistics/sorting-node.ini
```

비밀번호가 설정됐는지만 확인한다.

```bash
awk -F= \
  '/^password=/{if(length($2)>0)print "password: SET";else print "password: EMPTY"}' \
  /etc/logistics/sorting-node.ini
```

## 9. MQTT TLS 검증

Sorting Pi에서 실행한다.

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

## 10. Sorting Node 서비스 활성화

프로비저닝이 완료된 후에만 서비스를 활성화한다.

```bash
systemctl enable --now logistics-sorting-node
```

서비스 상태를 확인한다.

```bash
systemctl status logistics-sorting-node --no-pager -l
```

최근 로그를 확인한다.

```bash
journalctl -u logistics-sorting-node -n 30 --no-pager
```

정상 로그:

```text
MQTT broker connected
command topics subscribed
connected: /dev/vedauart
online status and registration published
```

## 11. MQTT 상태 및 센서 검증

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
  -t 'device/PI-SORTING-01/status' \
  -t 'device/PI-SORTING-01/heartbeat' \
  -t 'device/PI-SORTING-01/event' \
  -t 'device/PI-SORTING-01/response' \
  -t 'device/PI-SORTING-01/error' \
  -v
```

정상 기준:

- `DEVICE_STATUS`의 상태가 `ONLINE`
- heartbeat가 5초 주기로 수신
- 센서 1·2·3의 `SENSOR_STATUS` 수신
- 센서 앞 물체에 따라 `CLEAR → DETECTED → CLEAR` 변화
- 유효한 `distanceCm` 수신
- 예상하지 않은 `ERROR_OCCURRED`가 지속 발생하지 않음

## 12. 재부팅 자동 복구 검증

Sorting Pi에서 실행한다.

```bash
reboot
```

재접속 후 확인한다.

```bash
systemctl status logistics-sorting-node --no-pager -l
```

재부팅 후에도 다음 항목이 자동 복구돼야 한다.

- Sorting Node systemd 서비스
- MQTT TLS 연결
- MQTT 명령 토픽 구독
- `/dev/vedauart` 연결
- ONLINE 상태와 장치 등록 발행
- heartbeat 및 센서 이벤트 발행

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
- 실제 `sorting-node.ini`는 Git에 저장하지 않는다.
- SSH 공개키 교체 시 `authorized_keys`를 변경하고 이미지를 다시 빌드한다.
- CA 교체 시 중앙 서버 원본과 장치 인증서의 SHA-256 지문을 비교한다.
- SSH 개인키를 분실하면 새 공개키를 포함한 이미지를 다시 빌드한다.
- 새로운 SD카드에서는 프로비저닝 완료 후 `systemctl enable --now`를 한 번 실행한다.
