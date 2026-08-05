SUMMARY = "Logistics input conveyor Raspberry Pi node"
DESCRIPTION = "MQTT TLS and VEDAUART bridge for the input conveyor controller"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=5fade8d5ca2f983f62c28fb123891a52"

SRC_URI = " \
    git://github.com/VEDA4-T4/logistics-automation.git;protocol=https;nobranch=1 \
    file://logistics-input-node.service \
    file://input-node.ini.example \
"

SRCREV = "7a579c5216971ca19a89a401cf7b919f05d86b7f"

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
    -DLOGISTICS_BUILD_INPUT_NODE=ON \
    -DLOGISTICS_BUILD_VISION_NODE=OFF \
    -DLOGISTICS_BUILD_SORTING_NODE=OFF \
    -DLOGISTICS_BUILD_LINETRACER_NODE=OFF \
"

SYSTEMD_SERVICE:${PN} = "logistics-input-node.service"
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
    logistics-input \
"

do_install() {
    install -d ${D}${bindir}

    install -m 0755 \
        ${B}/device-rpi/logistics_input_node \
        ${D}${bindir}/logistics_input_node

    install -d ${D}${sysconfdir}/logistics

    # SRCREV predates device-rpi/config/input-node.ini.example, so the template
    # ships with this recipe instead of coming from ${S}. Move it to ${S} once
    # SRCREV advances past the commit that adds the repository copy.
    install -m 0600 \
        ${WORKDIR}/input-node.ini.example \
        ${D}${sysconfdir}/logistics/input-node.ini.example

    install -d ${D}${systemd_system_unitdir}

    install -m 0644 \
        ${WORKDIR}/logistics-input-node.service \
        ${D}${systemd_system_unitdir}/logistics-input-node.service
}

RDEPENDS:${PN}:append = " ca-certificates"
