SUMMARY = "Logistics sorting conveyor Raspberry Pi node"
DESCRIPTION = "MQTT TLS and VEDAUART bridge for the sorting conveyor controller"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=5fade8d5ca2f983f62c28fb123891a52"

SRC_URI = " \
    git://github.com/VEDA4-T4/logistics-automation.git;protocol=https;nobranch=1 \
    file://logistics-sorting-node.service \
"

SRCREV = "7cb35bc6f4ae3a84efae1a3b398aa2b1eadc408c"

S = "${WORKDIR}/git"

inherit cmake pkgconfig systemd useradd

DEPENDS = " \
    curl \
    openssl \
    mosquitto \
    nlohmann-json \
"

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
    -DLOGISTICS_BUILD_DEVICE_NODES=ON \
    -DLOGISTICS_BUILD_DEVICE_UART_TRANSPORT=ON \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON \
    -DLOGISTICS_BUILD_INPUT_NODE=OFF \
    -DLOGISTICS_BUILD_VISION_NODE=OFF \
    -DLOGISTICS_BUILD_SORTING_NODE=ON \
    -DLOGISTICS_BUILD_GRIPPER_NODE=OFF \
    -DLOGISTICS_BUILD_LINETRACER_NODE=OFF \
"

SYSTEMD_SERVICE:${PN} = "logistics-sorting-node.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

USERADD_PACKAGES = "${PN}"

GROUPADD_PARAM:${PN} = "--system logistics"

USERADD_PARAM:${PN} = " \
    --system \
    --home-dir /var/lib/logistics \
    --no-create-home \
    --shell /sbin/nologin \
    --gid logistics \
    --groups dialout \
    logistics-sorting \
"

do_install() {
    install -d ${D}${bindir}

    install -m 0755 \
        ${B}/device-rpi/logistics_sorting_node \
        ${D}${bindir}/logistics_sorting_node

    install -d ${D}${sysconfdir}/logistics

    install -m 0600 \
        ${S}/device-rpi/config/sorting-node.ini.example \
        ${D}${sysconfdir}/logistics/sorting-node.ini.example

    install -d ${D}${systemd_system_unitdir}

    install -m 0644 \
        ${WORKDIR}/logistics-sorting-node.service \
        ${D}${systemd_system_unitdir}/logistics-sorting-node.service
}

RDEPENDS:${PN}:append = " ca-certificates"
