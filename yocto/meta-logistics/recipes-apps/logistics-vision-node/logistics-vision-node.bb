SUMMARY = "Logistics Raspberry Pi vision node"
DESCRIPTION = "IMX219 vision processing and MQTT TLS reporting for the logistics system"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=5fade8d5ca2f983f62c28fb123891a52"

SRC_URI = " \
    git://github.com/VEDA4-T4/logistics-automation.git;protocol=https;nobranch=1 \
    file://vision-node.ini.example \
    file://logistics-vision-node.service \
"

SRCREV = "8771e025180cac6395732aeaaf28755347f9b0eb"

S = "${WORKDIR}/git"

inherit cmake pkgconfig systemd useradd

DEPENDS = " \
    curl \
    openssl \
    mosquitto \
    nlohmann-json \
    opencv \
"

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DLOGISTICS_BUILD_CONTROL_CENTER=OFF \
    -DLOGISTICS_BUILD_CENTRAL_SERVER=OFF \
    -DLOGISTICS_BUILD_DEVICE_NODES=ON \
    -DLOGISTICS_BUILD_DEVICE_UART_TRANSPORT=OFF \
    -DLOGISTICS_ENABLE_MOSQUITTO_TRANSPORT=ON \
    -DLOGISTICS_BUILD_INPUT_NODE=OFF \
    -DLOGISTICS_BUILD_VISION_NODE=ON \
    -DLOGISTICS_BUILD_SORTING_NODE=OFF \
    -DLOGISTICS_BUILD_GRIPPER_NODE=OFF \
    -DLOGISTICS_BUILD_LINETRACER_NODE=OFF \
"

SYSTEMD_SERVICE:${PN} = "logistics-vision-node.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

USERADD_PACKAGES = "${PN}"

GROUPADD_PARAM:${PN} = "--system logistics"

USERADD_PARAM:${PN} = " \
    --system \
    --home-dir /var/lib/logistics \
    --no-create-home \
    --shell /sbin/nologin \
    --gid logistics \
    --groups video \
    logistics-vision \
"

do_install() {
    install -d ${D}${bindir}

    install -m 0755 \
        ${B}/device-rpi/logistics_vision_node \
        ${D}${bindir}/logistics_vision_node

    install -d ${D}${sysconfdir}/logistics

    install -m 0600 \
        ${WORKDIR}/vision-node.ini.example \
        ${D}${sysconfdir}/logistics/vision-node.ini.example

    install -d ${D}${systemd_system_unitdir}

    install -m 0644 \
        ${WORKDIR}/logistics-vision-node.service \
        ${D}${systemd_system_unitdir}/logistics-vision-node.service
}

RDEPENDS:${PN}:append = " ca-certificates"
