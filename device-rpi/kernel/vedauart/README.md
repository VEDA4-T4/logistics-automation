# VEDAUART serdev driver

Raspberry Pi 4의 UART1과 STM32 사이의 byte stream을 `/dev/vedauart`로 제공하는
out-of-tree Linux kernel module입니다. UART frame, CRC, ACK와 재시도는 사용자 공간에서 처리합니다.

## 대상 환경

- Raspberry Pi 4 / BCM2711
- Raspberry Pi OS Linux 6.1 이상
- UART1 GPIO14(TXD1, 물리 핀 8), GPIO15(RXD1, 물리 핀 10)
- 3.3V UART 및 공통 GND

Linux 6.1과 6.4 이상에서 달라진 `class_create()` API는 소스에서 조건부로 처리합니다.

## Raspberry Pi 설정

`/boot/firmware/config.txt`에 다음을 추가합니다.

```ini
enable_uart=1
dtoverlay=vedauart
```

UART1은 mini UART이므로 `enable_uart=1`로 core clock을 고정해야 115200 baud를 안정적으로 유지할 수
있습니다. `/boot/firmware/cmdline.txt`에서 `console=serial0,...` 또는 `console=ttyS0,...`를 제거하고 다음
서비스를 비활성화해 serial console과의 충돌을 방지합니다.

```sh
sudo systemctl disable --now serial-getty@serial0.service
sudo systemctl disable --now serial-getty@ttyS0.service
```

## 빌드 및 설치

실행 중인 Raspberry Pi kernel의 header와 Device Tree Compiler가 필요합니다.

```sh
sudo apt install raspberrypi-kernel-headers device-tree-compiler
make
sudo install -D -m 0644 vedauart.ko /lib/modules/$(uname -r)/extra/vedauart.ko
sudo depmod -a
sudo install -m 0644 vedauart.dtbo /boot/firmware/overlays/vedauart.dtbo
sudo install -m 0644 99-vedauart.rules /etc/udev/rules.d/99-vedauart.rules
sudo reboot
```

재부팅 후 다음 명령으로 확인합니다.

```sh
ls -l /dev/vedauart
modinfo vedauart
dmesg | grep vedauart
```
