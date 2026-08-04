SUMMARY = "Runtime packages for the logistics sorting node"
DESCRIPTION = "Sorting Node, VEDAUART, MQTT TLS runtime and diagnostic tools"
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    logistics-sorting-node \
    vedauart \
    kernel-module-vedauart \
    ca-certificates \
    openssl-bin \
    mosquitto-clients \
"
