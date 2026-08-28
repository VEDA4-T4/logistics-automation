SUMMARY = "Runtime packages for the logistics gripper node"
DESCRIPTION = "Gripper Node, VEDAUART, MQTT TLS runtime and diagnostic tools"
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    logistics-gripper-node \
    vedauart \
    kernel-module-vedauart \
    ca-certificates \
    openssl-bin \
    mosquitto-clients \
    logistics-production-access \
"
