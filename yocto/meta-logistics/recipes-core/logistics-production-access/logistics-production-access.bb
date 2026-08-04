SUMMARY = "Production SSH access for logistics devices"
DESCRIPTION = "Install the authorized root SSH public key for key-only device administration"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://authorized_keys"

S = "${WORKDIR}"

inherit allarch

do_install() {
    install -d -m 0700 \
        ${D}${ROOT_HOME}/.ssh

    install -m 0600 \
        ${WORKDIR}/authorized_keys \
        ${D}${ROOT_HOME}/.ssh/authorized_keys
}

FILES:${PN} = " \
    ${ROOT_HOME}/.ssh \
"

RDEPENDS:${PN} = "dropbear"
