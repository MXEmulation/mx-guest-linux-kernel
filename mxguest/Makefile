# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

ifneq ($(src),)
CORE_DIR ?= $(abspath $(src)/../deps/core)
obj-m := mxguest.o
core-sources := $(filter src/%.c,$(shell cat $(CORE_DIR)/sources.list))
core-objects := $(patsubst src/%.c,mxgacore_%.o,$(core-sources))
mxguest-objs := mxguest_drv.o $(core-objects)
targets += $(core-objects)

$(obj)/mxgacore_%.o: $(CORE_DIR)/src/%.c FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)
ccflags-y += -I$(src)/../mxgpu/ksys
ccflags-y += -I$(CORE_DIR)/include -I$(CORE_DIR)/src
else
KDIR ?= /lib/modules/$(shell uname -r)/build
CORE_DIR ?= $(abspath ../deps/core)
.PHONY: modules clean
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CORE_DIR="$(CORE_DIR)" modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
