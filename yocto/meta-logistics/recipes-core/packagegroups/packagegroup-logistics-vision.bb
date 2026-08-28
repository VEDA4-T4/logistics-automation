SUMMARY = "Runtime packages for the logistics vision node"
DESCRIPTION = "Vision Node, IMX219 camera, OpenCV, GStreamer and MQTT TLS runtime"
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    logistics-vision-node \
    libcamera \
    libcamera-gst \
    gstreamer1.0-meta-base \
    gstreamer1.0-plugins-base-app \
    ca-certificates \
    openssl-bin \
    mosquitto-clients \
    logistics-production-access \
"
