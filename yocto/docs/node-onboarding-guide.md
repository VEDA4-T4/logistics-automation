# 새 노드를 Yocto로 올리는 인수인계 가이드

작성: 2026-08-05. 대상 브랜치 `feature/yocto-bsp-poc`.

분류(sorting) 노드와 투입(input) 노드를 Yocto 제품 이미지로 만들어 실기기 검증까지 마친
과정을, **라인트레이서 · 비전 · 그리퍼 노드에 그대로 적용할 수 있도록** 정리한 문서입니다.

Yocto를 처음 접하는 사람이 환경 구축부터 순서대로 따라갈 수 있게 썼습니다.
각 단계마다 **무엇을 하는 것인지**와 **왜 그렇게 하는지**를 함께 적었습니다.

> 이론 배경이 궁금하면 `stm32/conveyor-controller/docs/yocto-buildroot-uboot-guide.md`를
> 먼저 읽으세요. 이 문서는 **손을 움직이는 절차**에 집중합니다.

```
목차
0단계. 시작 전 확인 — 내 노드는 지금 올릴 수 있는가
1단계. 빌드 환경 구축 (최초 1회)
2단계. 기존 이미지를 한 번 빌드해보기
3단계. 새 노드용 파일 5개 만들기
4단계. 빌드하고 SD카드에 굽기
5단계. 부팅 후 프로비저닝
6단계. 검증
7단계. 노드별 개별 주의사항
부록 A. 자주 만나는 문제
부록 B. 체크리스트
```

---

# 0단계. 시작 전 확인 — 내 노드는 지금 올릴 수 있는가

노드마다 준비 상태가 다릅니다. **여기서 막히면 1단계로 가도 소용없습니다.**

| 노드 | 소스가 main에 | CMake 옵션 | UART 사용 | 착수 가능? |
|---|---|---|---|---|
| **라인트레이서** | ✅ | ✅ `LOGISTICS_BUILD_LINETRACER_NODE` | ✅ | ✅ **바로 가능** |
| **비전** | ✅ | ✅ `LOGISTICS_BUILD_VISION_NODE` | ❌ | ⚠️ OpenCV 선결 |
| **그리퍼** | ❌ `feature/gripper-node`에만 | ❌ **없음** | ✅ | ❌ 선결 작업 필요 |

## 확인 명령

```bash
git fetch --all --prune
```

```bash
git ls-tree --name-only origin/main device-rpi/<노드>-node/
```

```bash
git show origin/main:CMakeLists.txt | grep LOGISTICS_BUILD
```

## 막혔다면 — 선결 작업

**그리퍼**: Yocto 이전에 두 가지가 먼저입니다.
1. `feature/gripper-node`를 `main`에 머지
2. 최상위 `CMakeLists.txt`에 `option(LOGISTICS_BUILD_GRIPPER_NODE ...)` 추가하고
   `device-rpi/CMakeLists.txt`에 타깃 등록

**비전**: OpenCV 버전 충돌을 먼저 풀어야 합니다.

```
저장소 요구:   find_package(OpenCV 4.10.0 EXACT ...)   ← device-rpi/CMakeLists.txt:142
scarthgap 제공: opencv_4.9.0.bb
```

`EXACT`가 붙어 있는 한 빌드가 시작조차 안 됩니다. 7단계에 선택지를 정리했습니다.

**왜 이걸 먼저 보나**: Yocto 레시피는 `SRCREV`로 **GitHub의 특정 커밋**을 받아 빌드합니다.
내 로컬에만 있는 코드는 이미지에 들어가지 않습니다.

---

# 1단계. 빌드 환경 구축 (최초 1회)

## 1-1. 왜 WSL2인가

Yocto(bitbake)는 리눅스에서만 돕니다. Windows에서 개발한다면 WSL2가 가장 단순합니다.

## ⚠️ 1-2. 가장 흔한 실수 — `/mnt/c`에서 빌드

**WSL 안이라도 `/mnt/c/...` 경로에서는 빌드가 안 됩니다.**

- Windows 파일시스템은 대소문자를 구분하지 않음 → `Makefile`과 `makefile`이 충돌
- 심볼릭 링크·퍼미션이 제대로 표현되지 않음
- I/O가 수십 배 느림

반드시 리눅스 네이티브 경로(`/home/...`)를 쓰세요.

```bash
df -hT ~ | tail -1
```

`ext4`가 나와야 합니다. `9p`면 잘못된 위치입니다.

## 1-3. 호스트 패키지 설치

```bash
sudo apt update
```

```bash
sudo apt install -y gawk wget git diffstat unzip texinfo gcc build-essential chrpath socat cpio python3 python3-pip python3-pexpect xz-utils debianutils iputils-ping python3-git python3-jinja2 python3-subunit zstd liblz4-tool file locales libacl1 bmap-tools
```

```bash
sudo locale-gen en_US.UTF-8
```

진단·검증에 쓸 MQTT 클라이언트도 미리 넣어둡니다.

```bash
sudo apt install -y mosquitto-clients
```

**무엇을 하는 건가**: bitbake가 내부적으로 호출하는 도구들입니다. 하나라도 없으면
빌드 초반 sanity check에서 정확히 어떤 게 없는지 알려주며 멈춥니다.

## 1-4. kas 설치

kas는 레이어 clone과 `local.conf`/`bblayers.conf` 생성을 자동화하는 도구입니다.
이게 없으면 그 과정을 전부 손으로 해야 합니다.

Ubuntu 24.04는 PEP 668 때문에 `pip install`이 막혀 있어 `pipx`를 씁니다.

```bash
sudo apt install -y pipx
```

```bash
pipx install kas
```

```bash
pipx ensurepath
```

새 셸을 열고 확인:

```bash
kas --version
```

## 1-5. 디렉터리 배치

팀 공통 구조입니다. **이 구조를 지키면 문서의 명령을 그대로 복사해 쓸 수 있습니다.**

```
~/workspace/logistics-automation/    Git 저장소 (코드·레시피)
~/yocto-work/logistics/              빌드 작업 공간 (Git 밖)
   ├── poky/  meta-raspberrypi/  meta-openembedded/   kas가 clone
   ├── downloads/        소스 캐시    ← 모든 이미지가 공유
   ├── sstate-cache/     빌드 캐시    ← 모든 이미지가 공유
   ├── build-sorting/
   ├── build-input/
   └── build-<내노드>/
```

```bash
mkdir -p ~/workspace ~/yocto-work/logistics
```

```bash
git clone https://github.com/VEDA4-T4/logistics-automation.git ~/workspace/logistics-automation
```

```bash
cd ~/workspace/logistics-automation && git checkout feature/yocto-bsp-poc
```

**왜 빌드 공간을 저장소 밖에 두나**: 빌드 산출물은 수십 GB이고 Git에 들어가면 안 됩니다.
`downloads/`와 `sstate-cache/`를 형제 디렉터리로 두면 **모든 노드 이미지가 캐시를
공유**해서 두 번째 이미지부터 훨씬 빨리 빌드됩니다.

## 1-6. 디스크 여유 확인

```bash
df -h ~
```

**최소 100GB**를 권장합니다. 첫 빌드에서 poky·meta-openembedded clone과 툴체인 빌드에
많이 씁니다.

---

# 2단계. 기존 이미지를 한 번 빌드해보기

새 노드를 만들기 **전에** 이미 검증된 이미지를 한 번 빌드하세요.
환경 문제와 내가 만든 파일 문제를 분리할 수 있습니다.

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

**첫 빌드는 수 시간** 걸립니다 (레이어 clone + 툴체인 + 커널 전체 빌드).
캐시가 있으면 이후는 **10분 이내**입니다.

## 무엇이 일어나는가

```
kas build yocto/kas/input.yml
  │
  ├─ poky / meta-raspberrypi / meta-openembedded 를 clone·체크아웃
  │     (base.lock.yml에 적힌 커밋으로 정확히 고정)
  ├─ build-input/conf/local.conf, bblayers.conf 자동 생성
  └─ bitbake logistics-input-image 실행
       ├─ 레시피 2754개 파싱
       ├─ 태스크 5057개 의존성 계산
       ├─ sstate 캐시에 있는 건 건너뜀
       └─ rootfs 조립 → .wic.bz2
```

## 결과 확인

```bash
ls -la $KAS_BUILD_DIR/tmp/deploy/images/raspberrypi4-64/ | grep wic
```

`logistics-input-image-raspberrypi4-64.rootfs.wic.bz2`가 있으면 성공입니다.

---

# 3단계. 새 노드용 파일 5개 만들기

여기가 본론입니다. **`input` 노드를 그대로 베껴 쓰는 게 가장 안전합니다.**
아래에서는 라인트레이서(`linetracer`)를 예로 듭니다.

## 만들 파일

```
yocto/kas/linetracer.yml
yocto/meta-logistics/recipes-apps/logistics-linetracer-node/logistics-linetracer-node.bb
yocto/meta-logistics/recipes-apps/logistics-linetracer-node/files/logistics-linetracer-node.service
yocto/meta-logistics/recipes-core/packagegroups/packagegroup-logistics-linetracer.bb
yocto/meta-logistics/recipes-core/images/logistics-linetracer-image.bb
```

경우에 따라 추가:
```
device-rpi/config/linetracer-node.ini.example      ← 없으면 만들어야 함
yocto/docs/linetracer-node-provisioning.md         ← 배포 절차 문서
```

## 3-1. 설정 템플릿 확인 — 없으면 먼저 만들기

```bash
ls device-rpi/config/
```

`linetracer-node.ini.example`이 없다면 만들어야 합니다. 레시피가 이걸 이미지의
`/etc/logistics/`에 설치하고, 운영자가 부팅 후 복사해서 씁니다.

`input-node.ini.example`을 복사해 `device_id`, `node_name`만 바꾸면 됩니다.

> ⚠️ **파서가 아는 섹션은 `[device]`, `[mqtt]`, `[sorting]`, `[log_upload]`,
> `[image_upload]` 뿐입니다.** 그 외 섹션은 **에러가 아니라 조용히 무시**됩니다
> (`mqtt_node_config.cpp:233`). 이게 더 위험합니다 — `[mqqt]`처럼 오타를 내면
> 그 블록 전체가 무시되고, 나중에 "필수 값이 없다"는 엉뚱한 에러로 나타납니다.
> 반대로 **아는 섹션 안의 모르는 키는 명확한 에러**를 냅니다
> (`unknown [mqtt] setting: ...`). 즉 `[linetracer] speed=50` 같은 전용 섹션을
> 추가해도 무시될 뿐 반영되지 않습니다. 노드 전용 설정이 필요하면 파서에
> 섹션을 추가하는 코드 작업이 선행돼야 합니다.

## 3-2. 애플리케이션 레시피 (`.bb`)

`logistics-input-node.bb`를 복사해서 만듭니다. **바꿔야 할 곳은 5군데뿐입니다.**

```bitbake
SUMMARY = "Logistics line tracer Raspberry Pi node"          # ① 설명
DESCRIPTION = "MQTT TLS and VEDAUART bridge for the line tracer controller"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=5fade8d5ca2f983f62c28fb123891a52"

SRC_URI = " \
    git://github.com/VEDA4-T4/logistics-automation.git;protocol=https;nobranch=1 \
    file://logistics-linetracer-node.service \                # ② 유닛 파일명
"

SRCREV = "<빌드할 커밋 해시>"                                  # ③ 소스 리비전

S = "${WORKDIR}/git"

inherit cmake pkgconfig systemd useradd

DEPENDS = "curl openssl mosquitto nlohmann-json"

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
    -DLOGISTICS_BUILD_DEVICE_NODES=ON \
    -DLOGISTICS_BUILD_DEVICE_UART_TRANSPORT=ON \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON \
    -DLOGISTICS_BUILD_INPUT_NODE=OFF \
    -DLOGISTICS_BUILD_VISION_NODE=OFF \
    -DLOGISTICS_BUILD_SORTING_NODE=OFF \
    -DLOGISTICS_BUILD_LINETRACER_NODE=ON \                    # ④ 내 노드만 ON
"

SYSTEMD_SERVICE:${PN} = "logistics-linetracer-node.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "--system logistics"
USERADD_PARAM:${PN} = " \
    --system --home-dir /var/lib/logistics --no-create-home \
    --shell /sbin/nologin --gid logistics --groups dialout \
    logistics-linetracer \                                    # ⑤ 서비스 계정명
"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 \
        ${B}/device-rpi/logistics_linetracer_node \
        ${D}${bindir}/logistics_linetracer_node

    install -d ${D}${sysconfdir}/logistics
    install -m 0600 \
        ${S}/device-rpi/config/linetracer-node.ini.example \
        ${D}${sysconfdir}/logistics/linetracer-node.ini.example

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 \
        ${WORKDIR}/logistics-linetracer-node.service \
        ${D}${systemd_system_unitdir}/logistics-linetracer-node.service
}

RDEPENDS:${PN}:append = " ca-certificates"
```

### 각 부분이 하는 일

| 항목 | 의미 |
|---|---|
| `SRC_URI` + `SRCREV` | GitHub에서 **이 커밋**을 받아 빌드. 재현성의 핵심 |
| `nobranch=1` | 브랜치가 아닌 커밋 해시로 직접 지정 |
| `S = "${WORKDIR}/git"` | 받아온 소스가 풀리는 위치 |
| `inherit cmake` | CMake 빌드를 자동 처리 |
| `inherit systemd` | 유닛 등록 |
| `inherit useradd` | 서비스 전용 계정 생성 |
| `DEPENDS` | **빌드 시점** 의존성 (헤더/라이브러리) |
| `RDEPENDS` | **실행 시점** 의존성. 공유 라이브러리는 자동 감지되므로 안 적어도 됨 |
| `SYSTEMD_AUTO_ENABLE = "disable"` | **프로비저닝 전에는 서비스가 뜨지 않게** |
| `--groups dialout` | `/dev/vedauart`가 `dialout` 0660이라 필요 |
| `do_install` | `cmake.bbclass`의 기본 설치를 덮어씀 |

> ⚠️ **`SRCREV`가 가리키는 커밋에 파일이 있어야 합니다.**
> `${S}/device-rpi/config/linetracer-node.ini.example`을 설치하려면
> 그 커밋에 그 파일이 들어 있어야 합니다. 새로 만든 파일이라면
> **커밋 → 푸시 → SHA 확인 → SRCREV 갱신** 순서를 지켜야 합니다.

## 3-3. systemd 유닛

`logistics-input-node.service`를 복사해 이름과 경로만 바꿉니다.
**하드닝 설정은 그대로 두세요.**

```ini
[Unit]
Description=Logistics Line Tracer Node
Wants=network-online.target
After=network-online.target systemd-modules-load.service
ConditionPathExists=/etc/logistics/linetracer-node.ini
ConditionPathExists=/etc/logistics/tls/ca.crt

[Service]
Type=simple
User=logistics-linetracer
Group=logistics
SupplementaryGroups=dialout
WorkingDirectory=/var/lib/logistics
ExecStart=/usr/bin/logistics_linetracer_node /etc/logistics/linetracer-node.ini /dev/vedauart
Restart=on-failure
RestartSec=2
StateDirectory=logistics
UMask=0077
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict
LockPersonality=true
MemoryDenyWriteExecute=true
ProtectClock=true
ProtectControlGroups=true
ProtectHostname=true
ProtectKernelLogs=true
ProtectKernelModules=true
ProtectKernelTunables=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
RestrictRealtime=true
RestrictSUIDSGID=true
SystemCallArchitectures=native

[Install]
WantedBy=multi-user.target
```

**`ConditionPathExists`가 중요합니다**: 설정 파일과 CA가 없으면 서비스가 아예 시작하지
않습니다. 프로비저닝 전에 서비스가 돌면서 오류를 뿜는 상황을 막습니다.

**실행 인자 확인**: 노드마다 인자 구조가 같은지 확인하세요.

```bash
grep -n 'usage:' device-rpi/linetracer-node/main.cpp
# → usage: logistics_linetracer_node [node.ini] [/dev/vedauart]
```

UART를 쓰지 않는 노드(비전)라면 두 번째 인자와 `SupplementaryGroups=dialout`을 빼야 합니다.

## 3-4. packagegroup

이미지에 들어갈 런타임 묶음입니다.

```bitbake
SUMMARY = "Runtime packages for the logistics line tracer node"
DESCRIPTION = "Line tracer Node, VEDAUART, MQTT TLS runtime and diagnostic tools"
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    logistics-linetracer-node \
    vedauart \
    kernel-module-vedauart \
    ca-certificates \
    openssl-bin \
    mosquitto-clients \
    logistics-production-access \
"
```

| 항목 | 왜 필요한가 |
|---|---|
| `vedauart` + `kernel-module-vedauart` | UART 드라이버. **UART 안 쓰는 노드는 제외** |
| `ca-certificates` | TLS |
| `openssl-bin` | 현장에서 `openssl s_client`로 진단 |
| `mosquitto-clients` | 현장에서 `mosquitto_pub`로 진단 |
| `logistics-production-access` | root SSH 공개키 |

## 3-5. 이미지 레시피

```bitbake
SUMMARY = "Logistics line tracer Raspberry Pi image"
DESCRIPTION = "Bootable Raspberry Pi image for the TLS-enabled line tracer node"
LICENSE = "MIT"

require recipes-core/images/include/logistics-base.inc
require recipes-core/images/include/logistics-vedauart.inc
require recipes-core/images/include/logistics-production-ssh.inc

IMAGE_INSTALL:append = " \
    packagegroup-logistics-linetracer \
"

IMAGE_ROOTFS_EXTRA_SPACE:append = " + 262144"
```

**`require`하는 세 파일이 하는 일**:

| 파일 | 역할 |
|---|---|
| `logistics-base.inc` | `core-image-minimal` + Wi-Fi 프로비저닝 |
| `logistics-vedauart.inc` | `vedauart.dtbo`를 부트 파티션에 배치. **UART 안 쓰면 제외** |
| `logistics-production-ssh.inc` | SSH 키 전용 강제. 키가 비면 **빌드 중단** |

`IMAGE_ROOTFS_EXTRA_SPACE`는 KB 단위 여유 공간입니다 (262144 = 256MB).

## 3-6. kas 타깃

```yaml
header:
  version: 23
  includes:
    - yocto/kas/base.yml

target:
  - logistics-linetracer-image

local_conf_header:
  linetracer-vedauart: |
    RPI_EXTRA_CONFIG:append = "\n# Enable VEDAUART\ndtoverlay=vedauart\n"

  production-security: |
    EXTRA_IMAGE_FEATURES += "allow-root-login"

  production-identity: |
    hostname:pn-base-files = "linetracer-node-01"
```

| 항목 | 의미 |
|---|---|
| `includes: base.yml` | 레이어 목록·MACHINE 등 공통 설정 상속 |
| `target` | 빌드할 이미지 레시피 이름 |
| `RPI_EXTRA_CONFIG` | `config.txt`에 추가될 내용. **UART 안 쓰면 제외** |
| `allow-root-login` | root SSH 허용 (비밀번호는 여전히 잠김) |
| `hostname:pn-base-files` | 장치 hostname |

> ⚠️ `debug-tweaks`는 **절대 넣지 마세요.** root 빈 비밀번호가 됩니다.

## 3-7. SSH 공개키 등록

이미지에 들어갈 공개키가 없으면 **부팅해도 접속할 수 없습니다.**
(비밀번호가 잠겨 있어 콘솔로도 못 들어갑니다.)

```bash
cat yocto/meta-logistics/recipes-core/logistics-production-access/files/authorized_keys
```

본인 키가 없으면 추가하세요.

```bash
ssh-keygen -t ed25519 -a 100 -f ~/.ssh/logistics_yocto_admin -C "본인이름-logistics-admin"
```

```bash
cat ~/.ssh/logistics_yocto_admin.pub >> yocto/meta-logistics/recipes-core/logistics-production-access/files/authorized_keys
```

**이 파일은 모든 노드 이미지가 공유합니다.** 팀원과 합의하고 커밋하세요.

---

# 4단계. 빌드하고 SD카드에 굽기

## 4-1. 커밋과 푸시가 먼저

레시피의 `SRCREV`는 **GitHub의 커밋**을 가리킵니다. 푸시하지 않은 코드는 빌드되지 않습니다.

```bash
git add device-rpi/config/linetracer-node.ini.example
git commit -m "build: add line tracer node config template"
git push origin feature/yocto-bsp-poc
```

```bash
git rev-parse HEAD
```

출력된 해시를 레시피의 `SRCREV`에 넣고 다시 커밋·푸시합니다.

**왜 두 번 커밋하나**: `SRCREV`가 가리킬 커밋이 먼저 존재해야 하기 때문입니다.
닭과 달걀 문제라 두 단계로 나눌 수밖에 없습니다.

## 4-2. 빌드

```bash
export KAS_WORK_DIR="$HOME/yocto-work/logistics"
export KAS_BUILD_DIR="$HOME/yocto-work/logistics/build-linetracer"
```

```bash
kas build yocto/kas/linetracer.yml
```

`build-linetracer`로 디렉터리를 분리하면 다른 노드 이미지와 섞이지 않습니다.
캐시(`downloads/`, `sstate-cache/`)는 공유되므로 **두 번째 노드부터는 빠릅니다.**

## 4-3. 이미지가 제대로 만들어졌는지 확인

```bash
grep -E 'logistics|mosquitto' $KAS_BUILD_DIR/tmp/deploy/images/raspberrypi4-64/*.manifest
```

`logistics-linetracer-node`, `logistics-production-access`, `vedauart`가 보여야 합니다.

## 4-4. 압축 해제 후 Windows로 복사

```bash
DEPLOY_DIR="$KAS_BUILD_DIR/tmp/deploy/images/raspberrypi4-64"
```

```bash
IMAGE_BZ2=$(readlink -f "$DEPLOY_DIR/logistics-linetracer-image-raspberrypi4-64.rootfs.wic.bz2")
```

```bash
bzip2 -dc "$IMAGE_BZ2" > /tmp/logistics-linetracer-production.wic
```

```bash
cp /tmp/logistics-linetracer-production.wic /mnt/c/Users/사용자명/Downloads/logistics-linetracer-production.img
```

```bash
sha256sum /tmp/logistics-linetracer-production.wic /mnt/c/Users/사용자명/Downloads/logistics-linetracer-production.img
```

**두 SHA-256이 일치해야 합니다.** 복사 중 손상을 잡습니다.

## 4-5. SD카드 굽기

Raspberry Pi Imager:
1. `Use custom` 선택
2. `.img` 파일 선택
3. SD카드 선택
4. **사용자·비밀번호·Wi-Fi 커스터마이징은 적용하지 않음** — 이미지가 자체 정책을 가짐
5. 기록 후 Pi에 장착하고 부팅

> 💡 **기존 SD카드는 보관하세요.** 문제가 생기면 카드만 바꿔 끼워 즉시 되돌릴 수 있습니다.

---

# 5단계. 부팅 후 프로비저닝

이미지에는 **비밀 정보와 환경별 설정이 들어 있지 않습니다.** 부팅 후 넣어야 합니다.

| 항목 | 왜 이미지에 없나 |
|---|---|
| CA 인증서 | 환경마다 다르고 교체됨 |
| MQTT 비밀번호 | 비밀 정보 |
| 장치 IP / 브로커 IP | 환경마다 다름 |

## 5-1. SSH 접속

```bash
ssh -o IdentitiesOnly=yes -i ~/.ssh/logistics_yocto_admin root@장치_IP
```

hostname이 kas에 적은 값(`linetracer-node-01`)으로 나오면 정상입니다.

장치 IP는 콘솔에서 확인합니다. **`hostname -I`는 busybox에 없습니다.**

```bash
ip addr
```

## 5-2. CA 인증서 설치

장치가 중앙 서버에 직접 접근 가능하면 한 번에 됩니다.

```bash
scp server@172.20.33.72:/etc/logistics/tls/ca.crt /tmp/ca.crt
```

```bash
mkdir -p /etc/logistics/tls && cp /tmp/ca.crt /etc/logistics/tls/ca.crt && chown root:root /etc/logistics/tls/ca.crt && chmod 0644 /etc/logistics/tls/ca.crt && rm /tmp/ca.crt
```

**지문을 원본과 대조하세요.** 중간자 공격 방어의 핵심입니다.

```bash
openssl x509 -in /etc/logistics/tls/ca.crt -noout -fingerprint -sha256
```

```bash
ssh server@172.20.33.72 'openssl x509 -in /etc/logistics/tls/ca.crt -noout -fingerprint -sha256'
```

## 5-3. 설정 파일

```bash
cp /etc/logistics/linetracer-node.ini.example /etc/logistics/linetracer-node.ini
```

IP만 일괄 치환합니다.

```bash
sed -i -e 's|^ip_address=.*|ip_address=장치_IP|' -e 's|^host=.*|host=172.20.33.72|' /etc/logistics/linetracer-node.ini
```

> ⚠️ **`host=`는 서버 인증서 SAN에 있는 값이어야 합니다.**
> 현재 SAN은 `IP:172.20.33.72, DNS:raspberrypi`이고 `mqtt.logistics.local`은 **없습니다.**
> 템플릿 기본값이 `mqtt.logistics.local`이라 반드시 바꿔야 합니다.
> ```bash
> openssl x509 -in /etc/logistics/tls/ca.crt -noout -ext subjectAltName   # 확인용
> ```

비밀번호는 **편집기로 직접** 입력합니다. 명령줄에 쓰면 셸 히스토리에 남습니다.

```bash
vi /etc/logistics/linetracer-node.ini
```

busybox vi: `/password` 검색 → `i` 편집 → `Esc` → `:wq`

권한을 적용합니다. 서비스 계정이 `logistics` 그룹으로 읽습니다.

```bash
chown root:logistics /etc/logistics/linetracer-node.ini && chmod 0640 /etc/logistics/linetracer-node.ini
```

확인 (비밀번호 값은 출력하지 않음):

```bash
grep -E '^(device_id|ip_address|host|port|client_id|username|tls_enabled|ca_certificate)=' /etc/logistics/linetracer-node.ini
```

## 5-4. 서비스 활성화

**TLS를 먼저 확인하고** 켭니다.

```bash
openssl s_client -connect 172.20.33.72:8883 -CAfile /etc/logistics/tls/ca.crt -verify_ip 172.20.33.72 -brief </dev/null
```

`Verification: OK`가 나와야 합니다.

```bash
systemctl enable --now logistics-linetracer-node
```

```bash
journalctl -u logistics-linetracer-node -n 30 --no-pager
```

정상 로그:

```
MQTT connection started for 172.20.33.72:8883
daemon started: id=PI-LT-01; uart=/dev/vedauart
connected: /dev/vedauart
MQTT broker connected; command topics subscribed; online status and registration published
```

---

# 6단계. 검증

## 6-1. 설치 확인 (장치에서)

```bash
ls -l /usr/bin/logistics_linetracer_node /etc/logistics/linetracer-node.ini.example
```

```bash
id logistics-linetracer
```

```bash
lsmod | grep vedauart && ls -l /dev/vedauart
```

## 6-2. 보안 확인

```bash
awk -F: '$1=="root"{if($2~/^[!*]/)print "root password: LOCKED";else print "root password: NOT LOCKED"}' /etc/shadow
```

```bash
cat /etc/default/dropbear
```

```bash
stat -c '%a %U:%G %n' /root/.ssh /root/.ssh/authorized_keys
```

기대값: `LOCKED`, `DROPBEAR_EXTRA_ARGS="-s"`, `700`/`600`.

## 6-3. 중앙서버에서 메시지 수신 확인 (WSL에서)

**장치에서 자기 토픽을 구독하면 안 됩니다.** ACL상 노드 계정은 자기 status/heartbeat에
write 권한만 있어서, 구독해도 아무것도 안 들어옵니다. `central-server` 계정이 필요합니다.

```bash
mkdir -p ~/.config/logistics && scp server@172.20.33.72:/etc/logistics/tls/ca.crt ~/.config/logistics/ca.crt
```

```bash
(
  cleanup() { unset MQTT_PW; }
  trap cleanup EXIT
  trap 'exit 130' HUP INT TERM
  read -rsp 'central-server MQTT password: ' MQTT_PW; printf '\n'
  mosquitto_sub -h 172.20.33.72 -p 8883 --cafile ~/.config/logistics/ca.crt \
    -u central-server -P "${MQTT_PW}" -i central-server-watch \
    -t 'device/PI-LT-01/#' -v
)
```

> ⚠️ **`-i`로 client_id를 분리하세요.** 실제 중앙서버 프로세스와 같은 ID로 붙으면
> 서로 밀어내서 중앙서버가 죽습니다.

## 6-4. 재부팅 자동 복구 ★ 가장 중요

수동으로 띄운 것과 "재부팅해도 스스로 복구되는 것"은 완성도가 전혀 다릅니다.

```bash
reboot
```

재접속 후:

```bash
systemctl status logistics-linetracer-node --no-pager -l
```

수동 개입 없이 서비스·MQTT TLS·토픽 구독·`/dev/vedauart`·ONLINE 발행이 전부
복구돼야 완료입니다.

---

# 7단계. 노드별 개별 주의사항

## 7-1. 라인트레이서 — 가장 쉬움

- 실행 인자가 input/sorting과 동일: `[node.ini] [/dev/vedauart]`
- UART를 쓰므로 vedauart 관련 설정 **전부 유지**
- `device_id`는 `PI-LT-01` (ACL 문서 기준)
- **필요한 추가 작업**: `device-rpi/config/linetracer-node.ini.example` 신규 작성

## 7-2. 비전 — OpenCV를 먼저 풀어야 함

**차단 요인**:

```
저장소 요구:   find_package(OpenCV 4.10.0 EXACT ...)
scarthgap 제공: opencv_4.9.0.bb
```

선택지:

| 방법 | 내용 | 평가 |
|---|---|---|
| **A. EXACT 완화** | `find_package(OpenCV 4.9 REQUIRED ...)` | ✅ **권장.** Yocto와 무관하게도 옳음 |
| B. bbappend로 4.10 올림 | meta-oe 레시피 오버라이드 | 패치·빌드 실패 위험, 유지보수 부담 |
| C. 비전만 기존 방식 유지 | Raspberry Pi OS 그대로 | 이행 목적에 반함 |

A를 택한다면 4.10 전용 API를 쓰는지 먼저 확인하세요.

**비전 노드의 다른 차이점**:
- **UART를 쓰지 않습니다** → 아래를 전부 빼세요
  - packagegroup에서 `vedauart`, `kernel-module-vedauart`
  - 이미지에서 `require ... logistics-vedauart.inc`
  - kas에서 `RPI_EXTRA_CONFIG` dtoverlay
  - 유닛에서 `SupplementaryGroups=dialout`과 두 번째 실행 인자
- 카메라 접근에 `video` 그룹이 필요할 수 있습니다
- 이미지 업로드를 쓰면 `[image_upload]` 섹션 설정이 필요합니다
- **OpenCV가 들어가면 이미지가 3~4배 커지고 빌드 시간이 크게 늘어납니다**

## 7-3. 그리퍼 — 선결 작업 두 가지

1. **소스 머지**: `feature/gripper-node` → `main` (또는 SRCREV가 그 브랜치를 가리키게)
2. **CMake 옵션 추가**: `LOGISTICS_BUILD_GRIPPER_NODE`가 **아예 없습니다**

```cmake
# 최상위 CMakeLists.txt
option(LOGISTICS_BUILD_GRIPPER_NODE "Build the gripper Raspberry Pi node" ON)
```

`device-rpi/CMakeLists.txt`에도 타깃 등록이 필요합니다.

그 뒤로는 라인트레이서와 동일합니다 (UART 사용).

---

# 부록 A. 자주 만나는 문제

### A-1. `/mnt/c`에서 빌드 → sanity check 실패 또는 극도로 느림

리눅스 네이티브 경로에서만 빌드하세요. `df -hT ~`가 `ext4`여야 합니다.

### A-2. `error: externally-managed-environment`

Ubuntu 24.04의 PEP 668. `pipx install kas`를 쓰세요.

### A-3. `do_install`에서 파일을 못 찾음

`SRCREV`가 가리키는 커밋에 그 파일이 없습니다. **커밋 → 푸시 → SHA 확인 → SRCREV 갱신**
순서를 지켰는지 확인하세요.

### A-4. `QA Issue: ... installed but not shipped`

`do_install`이 설치했는데 `FILES:${PN}`에 없는 경로가 있습니다.

```bitbake
FILES:${PN}:append = " ${sysconfdir}/무언가"
```

### A-5. 이미지 빌드가 `bbfatal`로 중단 — authorized_keys

```
bbfatal "Production SSH authorized_keys is missing or empty"
```

공개키 파일이 비어 있습니다. **의도된 동작입니다** — 접속 불가능한 이미지가 배포되는 걸
막습니다. 3-7절대로 키를 넣으세요.

### A-6. 부팅 후 SSH 접속 불가 (`Permission denied (publickey)`)

내 공개키가 이미지에 없습니다. 비밀번호가 잠겨 있어 **콘솔로도 복구할 수 없습니다.**
키를 추가하고 재빌드하거나, SD카드를 다른 리눅스에 마운트해
`/root/.ssh/authorized_keys`에 직접 넣어야 합니다.

### A-7. TLS는 되는데 MQTT 인증 실패

`Connection Refused: not authorised` → 비밀번호 또는 ACL 문제입니다. TLS 문제가 아닙니다.

### A-8. `disconnected (7)` 무한루프

같은 `client_id`로 다른 기기가 붙어 있습니다. **증상이 ACL 거부와 똑같이 보입니다.**

```bash
ssh server@172.20.33.72 'sudo ss -tnp | grep ":8883"'
```

### A-9. hostname 검증 실패

`.ini`의 `host=`가 서버 인증서 SAN에 없습니다. 5-3절 참조.

### A-10. 백그라운드 빌드 로그가 멈춤

bitbake는 데몬 구조라 호출한 셸이 끊겨도 계속 빌드합니다.

```bash
pgrep -af bitbake
```

```bash
tail -f $KAS_BUILD_DIR/bitbake-cookerdaemon.log
```

### A-11. 설정 파일 로드 실패

두 가지 경우가 다르게 동작합니다.

- **모르는 섹션** → 조용히 무시됨. 오타(`[mqqt]`)를 내면 그 블록이 통째로 사라지고
  "필수 값 없음"으로 나중에 터집니다. 섹션 이름 철자를 먼저 확인하세요.
- **아는 섹션의 모르는 키** → `unknown [mqtt] setting: ...` 로 명확히 실패합니다.

허용 섹션: `[device]`, `[mqtt]`, `[sorting]`, `[log_upload]`, `[image_upload]`.

---

# 부록 B. 체크리스트

## 착수 전
- [ ] 노드 소스가 `main`(또는 SRCREV 대상 브랜치)에 있는가
- [ ] `LOGISTICS_BUILD_<노드>_NODE` CMake 옵션이 있는가
- [ ] 이 노드가 UART를 쓰는가 (vedauart 포함 여부 결정)
- [ ] `device-rpi/config/<노드>-node.ini.example`이 있는가

## 환경
- [ ] WSL2 리눅스 네이티브 경로 (`df -hT ~` → ext4)
- [ ] 디스크 100GB 이상
- [ ] 호스트 패키지 + `mosquitto-clients` 설치
- [ ] `kas --version` 동작
- [ ] `~/workspace` / `~/yocto-work/logistics` 구조
- [ ] 기존 이미지(input 또는 sorting) 빌드 성공

## 파일 작성
- [ ] `yocto/kas/<노드>.yml`
- [ ] `recipes-apps/logistics-<노드>-node/logistics-<노드>-node.bb`
- [ ] `recipes-apps/.../files/logistics-<노드>-node.service`
- [ ] `recipes-core/packagegroups/packagegroup-logistics-<노드>.bb`
- [ ] `recipes-core/images/logistics-<노드>-image.bb`
- [ ] CMake 옵션에서 **내 노드만 ON**
- [ ] 서비스 계정명·hostname·device_id가 노드에 맞게 변경됨
- [ ] `SYSTEMD_AUTO_ENABLE = "disable"`
- [ ] `debug-tweaks`를 넣지 **않았음**
- [ ] 내 SSH 공개키가 `authorized_keys`에 있음

## 빌드
- [ ] 코드 커밋·푸시 완료
- [ ] `SRCREV`가 푸시된 커밋을 가리킴
- [ ] `KAS_BUILD_DIR`을 노드별로 분리
- [ ] 매니페스트에 내 노드 패키지가 보임
- [ ] `.wic.bz2` 압축 해제 후 SHA-256 일치

## 실기기
- [ ] hostname이 의도한 값
- [ ] `lsmod | grep vedauart` (UART 노드만)
- [ ] `/dev/vedauart` 존재 (UART 노드만)
- [ ] `systemctl is-enabled` → `disabled` (프로비저닝 전)
- [ ] CA 지문이 중앙서버 원본과 일치
- [ ] `host=`가 인증서 SAN에 있는 값
- [ ] `openssl s_client` → `Verification: OK`
- [ ] 서비스 기동 후 정상 로그 4줄
- [ ] 중앙서버에서 register/status/heartbeat 수신
- [ ] **재부팅 후 자동 복구** ★
- [ ] root LOCKED, dropbear `-s`, 키 권한 700/600

## 마무리
- [ ] `yocto/docs/<노드>-node-provisioning.md` 작성
- [ ] 커밋 (`build:` / `security:` / `docs:` 접두어)
- [ ] 팀원에게 공유 브랜치 변경 알림
