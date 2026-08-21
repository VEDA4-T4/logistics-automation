SUMMARY = "First-boot Wi-Fi provisioning for logistics Raspberry Pi nodes"
DESCRIPTION = "Import Wi-Fi credentials from the Boot partition and configure wlan0"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://logistics-wifi.conf.example \
    file://logistics-wifi-provision \
    file://logistics-wifi-provision.service \
    file://20-wlan0.network \
"

S = "${WORKDIR}"

inherit allarch deploy systemd

SYSTEMD_SERVICE:${PN} = "logistics-wifi-provision.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${libexecdir}

    install -m 0755 \
        ${WORKDIR}/logistics-wifi-provision \
        ${D}${libexecdir}/logistics-wifi-provision

    install -d ${D}${systemd_system_unitdir}

    install -m 0644 \
        ${WORKDIR}/logistics-wifi-provision.service \
        ${D}${systemd_system_unitdir}/logistics-wifi-provision.service

    install -d ${D}${systemd_unitdir}/network

    install -m 0644 \
        ${WORKDIR}/20-wlan0.network \
        ${D}${systemd_unitdir}/network/20-wlan0.network
}

do_deploy() {
    install -d ${DEPLOYDIR}

    install -m 0644 \
        ${WORKDIR}/logistics-wifi.conf.example \
        ${DEPLOYDIR}/logistics-wifi.conf.example
}

addtask deploy after do_unpack before do_build

RDEPENDS:${PN} = " \
    wpa-supplicant \
    iw \
    linux-firmware-rpidistro-bcm43455 \
    wireless-regdb-static \
    kernel-module-brcmfmac \
    kernel-module-brcmfmac-wcc \
    avahi-daemon \
"

FILES:${PN}:append = " \
    ${libexecdir}/logistics-wifi-provision \
    ${systemd_unitdir}/network/20-wlan0.network \
"
