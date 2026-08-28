SUMMARY = "Logistics Raspberry Pi vision image"
DESCRIPTION = "Bootable Raspberry Pi image for the IMX219 and TLS-enabled vision node"
LICENSE = "MIT"

require recipes-core/images/include/logistics-base.inc
require recipes-core/images/include/logistics-production-ssh.inc

IMAGE_INSTALL:append = " \
    packagegroup-logistics-vision \
"

IMAGE_ROOTFS_EXTRA_SPACE:append = " + 1048576"
