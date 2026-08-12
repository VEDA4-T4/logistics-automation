RPI_EXTRA_IMAGE_BOOT_FILES:append = " \
    vedauart.dtbo;overlays/vedauart.dtbo \
"


do_image_wic[depends] += "vedauart:do_deploy"
