SUMMARY = "Logistics robotic gripper Raspberry Pi image"
DESCRIPTION = "Bootable Raspberry Pi image for the TLS-enabled robotic gripper node"
LICENSE = "MIT"

require recipes-core/images/include/logistics-base.inc
require recipes-core/images/include/logistics-vedauart.inc
require recipes-core/images/include/logistics-production-ssh.inc

IMAGE_INSTALL:append = " \
    packagegroup-logistics-gripper \
"

IMAGE_ROOTFS_EXTRA_SPACE:append = " + 262144"
