# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke

ifneq ($(KERNELRELEASE),)
CORE_DIR ?= $(abspath $(src)/../deps/core)
LINUX_COMMON_DIR ?= $(abspath $(src)/../deps/linux-common)
obj-m := mxgpu.o
core-sources := $(filter src/%.c,$(shell cat $(CORE_DIR)/sources.list))
common-sources := $(filter src/%.c,$(shell cat $(LINUX_COMMON_DIR)/sources.list))
core-objects := $(patsubst src/%.c,mxcore_%.o,$(core-sources))
common-objects := $(patsubst src/%.c,mxcommon_%.o,$(common-sources))
mxgpu-objs := mxgpu_drv.o $(core-objects) $(common-objects)
targets += $(core-objects) $(common-objects)

$(obj)/mxcore_%.o: $(CORE_DIR)/src/%.c FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/mxcommon_%.o: $(LINUX_COMMON_DIR)/src/%.c FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)
ccflags-y += -I$(src)/ksys
ccflags-y += -I$(CORE_DIR)/include -I$(CORE_DIR)/src
ccflags-y += -I$(LINUX_COMMON_DIR)/include -I$(LINUX_COMMON_DIR)/src
else
KDIR ?= /lib/modules/$(shell uname -r)/build
CORE_DIR ?= $(abspath ../deps/core)
LINUX_COMMON_DIR ?= $(abspath ../deps/linux-common)
.PHONY: modules clean
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CORE_DIR="$(CORE_DIR)" LINUX_COMMON_DIR="$(LINUX_COMMON_DIR)" modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
