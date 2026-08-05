require recipes-core/images/core-image-minimal.bb

IMAGE_INSTALL:append = " \
    logistics-wifi-provision \
"

RPI_EXTRA_IMAGE_BOOT_FILES:append = " \
    logistics-wifi.conf.example \
"

do_image_wic[depends] += " \
    logistics-wifi-provision:do_deploy \
"
