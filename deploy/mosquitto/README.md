# Mosquitto 보안 및 TLS

운영 권장 구성은 MQTT over TLS `8883`, 사용자 인증, topic ACL입니다. 인증서와 비밀번호 파일은 저장소에 커밋하지
않고 중앙 브로커의 `/etc/mosquitto` 아래에서 관리합니다.

## 현재 지원 상태

| 구성 요소 | 사용자/비밀번호 | MQTT TLS |
| --- | --- | --- |
| Mosquitto broker | 지원 | 지원 |
| Central Server libmosquitto client | 지원 | 지원 |
| Device Node libmosquitto client | 지원 | 지원 |
| Qt Control Center `QMqttClient` | 지원 | 지원 |
| 중앙서버 HTTP upload | bearer token | HTTPS 지원 |

중앙서버와 Device Node는 libmosquitto에 CA를 적용하며, Control Center는 `QSslConfiguration`과
`connectToHostEncrypted()`를 사용합니다. 세 클라이언트 모두 서버 인증서를 검증하고 TLS 1.2 이상을 사용합니다.

## 권장 보안 단계

1. 개발망: `1883` + 사용자/비밀번호 + ACL
2. 마이그레이션: 제한된 `1883`과 TLS `8883` 이중 listener
3. 모든 클라이언트 TLS 검증
4. 방화벽과 Mosquitto에서 `1883` 폐쇄
5. 필요할 때만 mTLS 추가

첫 적용은 서버 인증 TLS와 사용자/비밀번호를 함께 사용하는 구성이 관리하기 쉽습니다. mTLS는 장치별 인증서 발급과
폐기 절차가 준비된 뒤 선택합니다.

## 1. 패키지 설치

```sh
sudo apt update
sudo apt install -y mosquitto mosquitto-clients openssl
sudo systemctl enable --now mosquitto
```

버전을 기록합니다.

```sh
mosquitto -h | head -n 1
openssl version
```

## 2. 사용자 비밀번호

비밀번호는 장치마다 다르게 발급합니다.

```sh
sudo touch /etc/mosquitto/passwd
sudo chown root:mosquitto /etc/mosquitto/passwd
sudo chmod 0640 /etc/mosquitto/passwd
sudo mosquitto_passwd /etc/mosquitto/passwd central-server
sudo mosquitto_passwd /etc/mosquitto/passwd control-center
sudo mosquitto_passwd /etc/mosquitto/passwd PI-INPUT-01
sudo mosquitto_passwd /etc/mosquitto/passwd PI-VISION-01
sudo mosquitto_passwd /etc/mosquitto/passwd PI-GRIPPER-01
sudo mosquitto_passwd /etc/mosquitto/passwd PI-SORTING-01
sudo mosquitto_passwd /etc/mosquitto/passwd PI-LT-01
```

실제 INI의 `[mqtt] username/password`에 대응하는 값을 넣습니다. `client_id`도 장치마다 고유해야 합니다.

## 3. ACL

`/etc/mosquitto/acl` 예:

```text
user central-server
topic read server/request/+
topic read device/+/register
topic read device/+/response
topic read device/+/status
topic read device/+/event
topic read device/+/error
topic read device/+/heartbeat
topic write qt/+/response
topic write qt/+/status
topic write qt/+/event
topic write qt/+/error
topic write device/+/command
topic write system/broadcast/command
topic write server/status
topic write server/heartbeat

user control-center
topic write server/request/control-center
topic read qt/control-center/response
topic read qt/control-center/status
topic read qt/control-center/event
topic read qt/control-center/error

user PI-VISION-01
topic read device/PI-VISION-01/command
topic read system/broadcast/command
topic write device/PI-VISION-01/register
topic write device/PI-VISION-01/response
topic write device/PI-VISION-01/status
topic write device/PI-VISION-01/event
topic write device/PI-VISION-01/error
topic write device/PI-VISION-01/heartbeat
```

Input, Gripper, Sorting, Line Tracer도 Vision 블록을 복사해 자신의 장치 ID만 사용합니다. 초기 통합 중에는 ACL 로그를
확인하되 `topic readwrite #`로 장기간 우회하지 않습니다.

```sh
sudo chown root:mosquitto /etc/mosquitto/acl
sudo chmod 0640 /etc/mosquitto/acl
```

## 4. 내부 CA와 서버 인증서

클라이언트가 IP `192.168.0.10`과 DNS `mqtt.logistics.local` 중 무엇으로 접속할지 먼저 정합니다. 접속에 사용하는
이름/IP가 서버 인증서 SAN에 반드시 있어야 합니다.

CA 개인키는 broker에 둘 필요가 없습니다. 안전한 관리 PC 또는 오프라인 위치에서 생성합니다.

```sh
umask 077
mkdir -p logistics-ca
cd logistics-ca

openssl genrsa -out ca.key 4096
openssl req -x509 -new -sha256 -days 3650 \
  -key ca.key \
  -out ca.crt \
  -subj "/O=VEDA Logistics/CN=VEDA Logistics Root CA"

openssl genrsa -out mqtt-server.key 3072
openssl req -new \
  -key mqtt-server.key \
  -out mqtt-server.csr \
  -subj "/O=VEDA Logistics/CN=mqtt.logistics.local"
```

`mqtt-server.ext`:

```text
subjectAltName=DNS:mqtt.logistics.local,IP:192.168.0.10
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
```

서명:

```sh
openssl x509 -req -sha256 -days 825 \
  -in mqtt-server.csr \
  -CA ca.crt \
  -CAkey ca.key \
  -CAcreateserial \
  -out mqtt-server.crt \
  -extfile mqtt-server.ext

openssl verify -CAfile ca.crt mqtt-server.crt
openssl x509 -in mqtt-server.crt -noout -subject -issuer -dates -ext subjectAltName
```

CA 개인키 `ca.key`는 중앙서버로 복사하지 않습니다. broker에는 공개 CA 인증서, 서버 인증서와 서버 개인키만
배치합니다.

```sh
sudo install -d -o root -g mosquitto -m 0750 /etc/mosquitto/certs
sudo install -o root -g mosquitto -m 0644 ca.crt /etc/mosquitto/certs/ca.crt
sudo install -o root -g mosquitto -m 0644 mqtt-server.crt /etc/mosquitto/certs/server.crt
sudo install -o root -g mosquitto -m 0640 mqtt-server.key /etc/mosquitto/certs/server.key
```

공개 CA 인증서 `ca.crt`는 모든 클라이언트 기기에 안전하게 배포합니다. 서버 인증서와 서버 개인키는 배포하지
않습니다.

Linux:

```sh
sudo install -d -o root -g root -m 0755 /etc/logistics/tls
sudo install -o root -g root -m 0644 ca.crt /etc/logistics/tls/ca.crt
```

Windows Control Center는 예를 들어 `C:\ProgramData\Logistics\tls\ca.crt`처럼 일반 사용자가 덮어쓸 수 없는
위치에 둡니다.

## 5. Mosquitto 설정

Ubuntu 패키지의 `/etc/mosquitto/mosquitto.conf`가 다음 include를 포함하는지 먼저 확인합니다.

```conf
include_dir /etc/mosquitto/conf.d
```

`/etc/mosquitto/conf.d/10-security.conf`:

```conf
allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file /etc/mosquitto/acl

persistence true
persistence_location /var/lib/mosquitto/
log_dest syslog
connection_messages true
```

마이그레이션 동안 사용할 `/etc/mosquitto/conf.d/20-listeners.conf`:

```conf
# Temporary plaintext listener. Restrict this port with the firewall.
listener 1883 0.0.0.0
protocol mqtt

listener 8883 0.0.0.0
protocol mqtt
certfile /etc/mosquitto/certs/server.crt
keyfile /etc/mosquitto/certs/server.key
cafile /etc/mosquitto/certs/ca.crt
require_certificate false
tls_version tlsv1.2
```

`tls_version tlsv1.2`는 최소 버전이며 TLS 1.2와 1.3을 허용합니다. `certfile`과 `keyfile`이 서버 TLS를
활성화합니다. `require_certificate false`는 서버만 인증하고 기존 사용자/비밀번호를 계속 사용한다는 뜻입니다.

기본 설정에 다른 `listener`나 `port`가 선언되어 있으면 중복을 제거합니다.

마이그레이션 중에는 `1883`을 전체 네트워크에 공개하지 않고 아직 전환하지 않은 장치 IP만 허용합니다. UFW 예:

```sh
sudo ufw allow from 192.168.0.10 to any port 1883 proto tcp
sudo ufw allow from 192.168.0.21 to any port 1883 proto tcp
sudo ufw allow 8883/tcp
```

## 6. 설정 검증과 재시작

실행 중인 서비스와 foreground Mosquitto가 같은 포트를 차지하지 않도록 합니다.

```sh
sudo systemctl stop mosquitto
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v
```

오류 없이 listener가 열리면 `Ctrl-C` 후 서비스를 시작합니다.

```sh
sudo systemctl start mosquitto
sudo systemctl status mosquitto --no-pager
sudo journalctl -u mosquitto -n 100 --no-pager
ss -lnt | grep -E ':(1883|8883)\b'
```

## 7. TLS listener 검증

먼저 인증서 이름과 신뢰 체인을 검사합니다.

```sh
openssl s_client \
  -connect mqtt.logistics.local:8883 \
  -servername mqtt.logistics.local \
  -CAfile ca.crt \
  -verify_hostname mqtt.logistics.local \
  -verify_return_error
```

IP로 접속한다면 `-verify_hostname` 대신 `-verify_ip 192.168.0.10`을 사용하고 인증서 SAN에도 같은 IP가 있어야
합니다.

두 터미널에서 실제 MQTT publish/subscribe를 검사합니다.

중앙서버 계정으로 구독:

```sh
mosquitto_sub \
  -h mqtt.logistics.local -p 8883 \
  --cafile ca.crt \
  -u central-server -P 'central-server-password' \
  -t 'device/PI-VISION-01/status' -d
```

Vision 계정으로 발행:

```sh
mosquitto_pub \
  -h mqtt.logistics.local -p 8883 \
  --cafile ca.crt \
  -u PI-VISION-01 -P 'vision-password' \
  -t 'device/PI-VISION-01/status' \
  -m '{"tls":"ok"}' -q 1 -d
```

이 검사는 broker TLS와 ACL만 확인합니다. 애플리케이션 메시지 JSON 계약 검증은 중앙서버에서 별도로 수행합니다.

## 애플리케이션 MQTT TLS 설정

세 클라이언트는 다음 키를 지원합니다.

```ini
[mqtt]
host=mqtt.logistics.local
port=8883
tls_enabled=true
ca_certificate=/etc/logistics/tls/ca.crt
```

`tls_enabled=true`이면 CA 경로가 필수입니다. Linux는 `/etc/logistics/tls/ca.crt`, Windows Control Center는
`C:\ProgramData\Logistics\tls\ca.crt` 같은 운영체제별 절대 경로를 사용합니다. 실제 비밀번호는 추적하는 예제 파일이
아니라 런타임 INI에만 기록합니다. 설치 스크립트의 환경변수와 비밀번호 입력 방법은
[설치 스크립트 가이드](../scripts/README.md)를 참고합니다.

## TLS 적용 순서

1. 사용자/비밀번호와 ACL을 `1883`에서 먼저 검증합니다.
2. CA와 서버 인증서를 만들고 `8883` listener를 추가합니다.
3. `mosquitto_sub/pub`와 `openssl s_client`로 `8883`을 검증합니다.
4. Central Server를 `8883`으로 전환합니다.
5. Device Node를 하나씩 `8883`으로 전환합니다.
6. Control Center를 `8883`으로 전환합니다.
7. heartbeat, 명령, 작업 이벤트, 이미지 화면을 전체 시나리오로 검증합니다.
8. 방화벽에서 `1883` 접근을 차단합니다.
9. Mosquitto의 `1883` listener를 제거하고 재시작합니다.

전환 중 `1883`도 `allow_anonymous false`, password file, ACL을 그대로 사용하며 필요한 장치 IP에서만 접근하도록
방화벽으로 제한합니다. 평문 listener는 비밀번호를 암호화하지 않으므로 전환이 끝나면 반드시 제거합니다.

```sh
sudo ufw delete allow from 192.168.0.10 to any port 1883 proto tcp
sudo ufw delete allow from 192.168.0.21 to any port 1883 proto tcp
```

## 선택 사항: mTLS

모든 장치에 개별 클라이언트 인증서를 안전하게 배포하고 폐기할 수 있을 때만 적용합니다.

```conf
listener 8883 0.0.0.0
certfile /etc/mosquitto/certs/server.crt
keyfile /etc/mosquitto/certs/server.key
cafile /etc/mosquitto/certs/ca.crt
require_certificate true
use_identity_as_username true
tls_version tlsv1.2
```

이 구성에서는 클라이언트 인증서 CN이 ACL 사용자 이름과 일치해야 하며 해당 listener의 `password_file` 인증 대신
인증서 신원이 사용됩니다. Central Server, Device Node, Control Center 모두 클라이언트 인증서와 개인키 로딩을
구현해야 합니다.

## 인증서 갱신과 롤백

- 만료 30일 전부터 갱신 알림을 둡니다.
- 같은 CA로 새 서버 인증서를 발급하고 `server.crt/server.key`를 원자적으로 교체합니다.
- `sudo systemctl reload mosquitto` 후 새 인증서가 제공되는지 확인합니다. listener 종류 같은 일부 설정 변경은
  재시작이 필요할 수 있습니다.
- 장애 시 임시 `1883` listener를 전체 공개하지 말고 방화벽으로 제한된 관리망에서만 복구에 사용합니다.
- 인증서 검증을 끄거나 `allow_anonymous true`로 롤백하지 않습니다.

## HTTP TLS와의 차이

MQTT TLS와 중앙서버 HTTP upload HTTPS는 별도 listener와 별도 클라이언트 설정입니다. MQTT가 TLS라고 해서 이미지
업로드가 자동으로 HTTPS가 되지 않습니다. HTTP는 중앙서버 `[http] tls_enabled=true`와 Vision
`endpoint_url=https://...`, `ca_certificate`, `allow_insecure_http=false`를 함께 적용해야 합니다.

## 공식 참고 자료

- [Mosquitto configuration manual](https://mosquitto.org/man/mosquitto-conf-5.html)
- [libmosquitto TLS API](https://mosquitto.org/api/)
- [Qt QMqttClient](https://doc.qt.io/qt-6/qmqttclient.html)
