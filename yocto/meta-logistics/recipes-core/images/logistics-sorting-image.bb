SUMMARY = "Logistics sorting conveyor Raspberry Pi image"
DESCRIPTION = "Bootable Raspberry Pi image for the TLS-enabled sorting conveyor node"
LICENSE = "MIT"

require recipes-core/images/include/logistics-base.inc
require recipes-core/images/include/logistics-vedauart.inc

IMAGE_INSTALL:append = " \
    packagegroup-logistics-sorting \
"

IMAGE_ROOTFS_EXTRA_SPACE:append = " + 262144"

logistics_harden_production_ssh() {
    dropbear_config="${IMAGE_ROOTFS}${sysconfdir}/default/dropbear"
    authorized_keys="${IMAGE_ROOTFS}${ROOT_HOME}/.ssh/authorized_keys"

    if [ ! -f "$dropbear_config" ]; then
        bbfatal "Dropbear configuration is missing from the production image"
    fi

    if [ ! -s "$authorized_keys" ]; then
        bbfatal "Production SSH authorized_keys is missing or empty"
    fi

    if grep -q '^DROPBEAR_EXTRA_ARGS=' "$dropbear_config"; then
        sed -i \
            's/^DROPBEAR_EXTRA_ARGS=.*/DROPBEAR_EXTRA_ARGS="-s"/' \
            "$dropbear_config"
    else
        printf '\nDROPBEAR_EXTRA_ARGS="-s"\n' >> "$dropbear_config"
    fi

    chmod 0700 "${IMAGE_ROOTFS}${ROOT_HOME}/.ssh"
    chmod 0600 "$authorized_keys"
}

ROOTFS_POSTPROCESS_COMMAND += "logistics_harden_production_ssh; "
