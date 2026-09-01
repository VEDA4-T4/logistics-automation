SUMMARY = "Runtime packages for the logistics line tracer node"
DESCRIPTION = "Line tracer node, VEDAUART, MQTT TLS runtime and diagnostic tools"
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    logistics-linetracer-node \
    vedauart \
    kernel-module-vedauart \
    ca-certificates \
    openssl-bin \
    mosquitto-clients \
    logistics-production-access \
"
