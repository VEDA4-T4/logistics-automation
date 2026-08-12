# Key-only root SSH for logistics production images.
#
# Required by every logistics-*-image so the hardening cannot drift between
# products. The build fails rather than shipping an image that would accept a
# password or that carries no authorized key at all.

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
