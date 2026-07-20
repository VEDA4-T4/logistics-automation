# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= /lib/modules/$(shell uname -r)/build
DTC ?= dtc

.PHONY: all module overlay clean

all: module overlay

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

overlay:
	$(DTC) -@ -I dts -O dtb -o vedauart.dtbo vedauart-overlay.dts

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) vedauart.dtbo
