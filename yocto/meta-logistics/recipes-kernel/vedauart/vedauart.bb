SUMMARY = "VEDA serdev UART character device driver"
DESCRIPTION = "Out-of-tree UART serdev driver, Device Tree overlay and udev permissions"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://vedauart.c;beginline=1;endline=1;md5=fcab174c20ea2e2bc0be64b493708266"

inherit module deploy

DEPENDS += "dtc-native"

FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../device-rpi/kernel/vedauart:"

SRC_URI = " \
    file://vedauart.c \
    file://Kbuild \
    file://Makefile \
    file://vedauart-overlay.dts \
    file://99-vedauart.rules \
"

S = "${WORKDIR}"
B = "${S}"

MAKE_TARGETS = "module"

EXTRA_OEMAKE += " \
    KDIR=${STAGING_KERNEL_DIR} \
"

KERNEL_MODULE_AUTOLOAD += "vedauart"

do_compile:append() {
    ${STAGING_BINDIR_NATIVE}/dtc \
        -@ \
        -I dts \
        -O dtb \
        -o ${B}/vedauart.dtbo \
        ${S}/vedauart-overlay.dts
}

do_install() {
    install -d \
        ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra

    install -m 0644 \
        ${B}/vedauart.ko \
        ${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/extra/vedauart.ko

    install -d \
        ${D}${sysconfdir}/udev/rules.d

    install -m 0644 \
        ${S}/99-vedauart.rules \
        ${D}${sysconfdir}/udev/rules.d/99-vedauart.rules
}

do_deploy() {
    install -d ${DEPLOYDIR}

    install -m 0644 \
        ${B}/vedauart.dtbo \
        ${DEPLOYDIR}/vedauart.dtbo
}

addtask deploy after do_compile before do_build

FILES:${PN}:append = " \
    ${sysconfdir}/udev/rules.d/99-vedauart.rules \
"