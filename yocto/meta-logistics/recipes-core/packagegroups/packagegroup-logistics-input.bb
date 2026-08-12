SUMMARY = "Runtime packages for the logistics input node"
DESCRIPTION = "Input Node, VEDAUART, MQTT TLS runtime and diagnostic tools"
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    logistics-input-node \
    vedauart \
    kernel-module-vedauart \
    ca-certificates \
    openssl-bin \
    mosquitto-clients \
    logistics-production-access \
"
