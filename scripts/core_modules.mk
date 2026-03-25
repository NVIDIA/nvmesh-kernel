# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
#
# IB core module helpers (included from top-level Makefile after NVMESH_SRC_DIR is set).

# check_ib_core_symbols: Generate shell commands to check IB core module symbols
# Args: $(1) = module directory path, $(2) = symvers file path
define check_ib_core_symbols
set -e;\
for file in $(1)/*.ko*; do \
name=$$(basename $$file); \
base=$${name%.*}; \
echo "Checking module $$base symbols..."; scripts/compare_symvers.py --new ./Module.symvers --orig "$(2)" --module $$base; \
done;
endef

# archive_ib_core_modules: Archive IB core modules to a tarball
# Args: $(1) = module directory path, $(2) = symvers file path
define archive_ib_core_modules
find $(1) -type f \( -name '*.ko' -o -name '*.ko.*' -o -name '*.h' -o -name '*.c' -not -name '*.mod.c' -not -name '*.ko.cmd' \) \
-exec realpath --relative-to=$(1) {} \; | tar -zcf ib_core_modules.tar.gz -C $(1) -T -
endef

# setup_kernel_ib_core_modules: Setup Kernel IB core modules for building and checking
# Args: $(1) = module directory path, $(2) = description (e.g., "OFED 5.4" or "Kernel 6.12"),
#       $(3) = symvers file path (optional)
# Sets: REL_IB_CORE_MOD_DIR, INFO_CORE_MOD, CHECK_IB_CORE_MOD, and updates obj-m and configs
# Note: Must be called with $(eval $(call setup_kernel_ib_core_modules,...))
define setup_kernel_ib_core_modules
REL_IB_CORE_MOD_DIR := $$(shell dirname "$(1)")
obj-m += $$(REL_IB_CORE_MOD_DIR)/
INFO_CORE_MOD := Building IB Core Modules for $(2) from $$(REL_IB_CORE_MOD_DIR)
configs += $$(call nconfig_set_val,IB_CORE_MOD_DIR,$$(REL_IB_CORE_MOD_DIR))
ARC_IB_CORE_MOD := $$(call archive_ib_core_modules,$$(REL_IB_CORE_MOD_DIR))
ifneq ($(3),)
    INFO_CORE_MOD += (Checked against $(3))
    CHECK_IB_CORE_MOD := $$(call check_ib_core_symbols,$$(REL_IB_CORE_MOD_DIR),$(3))
else
    INFO_CORE_MOD += (Not Checked!)
endif
endef

# check_ofed_ib_core_modules: Check for OFED modules and setup if found
# Args: none (uses OFED_FULL_VER, OFED_VER, and OFED_SYMVERS from the calling context)
# Note: Must be called with $(eval $(call check_ofed_ib_core_modules))
define check_ofed_ib_core_modules
$$(info OFED_FULL_VER $(OFED_FULL_VER) OFED_VER $(OFED_VER))
OFED_MODS_SRC_DIR := $$(firstword $$(wildcard $(NVMESH_SRC_DIR)/ofeds/$(OFED_FULL_VER) $$(wildcard $(NVMESH_SRC_DIR)/ofeds/$(OFED_VER))))
$$(info PWD $(PWD) OFED_MODS_SRC_DIR $$(OFED_MODS_SRC_DIR))
ifneq ($$(OFED_MODS_SRC_DIR),)
    $$(info OFED_MODS_SRC_DIR exists)
    OFED_MODS_MAKEFILE := $$(wildcard $$(OFED_MODS_SRC_DIR)/drivers/infiniband/core/Makefile)
    ifneq ($$(OFED_MODS_MAKEFILE),)
        $$(info OFED_MODS_MAKEFILE $$(OFED_MODS_MAKEFILE) exists)
        REL_IB_CORE_MOD_DIR := $$(patsubst $(NVMESH_SRC_DIR)/%,%,$$(shell dirname "$$(OFED_MODS_MAKEFILE)"))
        obj-m += $$(REL_IB_CORE_MOD_DIR)/
        INFO_CORE_MOD := Building IB Core Modules for OFED $$(OFED_FULL_VER) from $$(REL_IB_CORE_MOD_DIR)
        configs += $$(call nconfig_set_val,IB_CORE_MOD_DIR,$$(REL_IB_CORE_MOD_DIR))
        ARC_IB_CORE_MOD := $$(call archive_ib_core_modules,$$(REL_IB_CORE_MOD_DIR))
        ifneq ($(OFED_SYMVERS),)
            INFO_CORE_MOD += (Checked against $(OFED_SYMVERS))
            CHECK_IB_CORE_MOD := $$(call check_ib_core_symbols,$$(REL_IB_CORE_MOD_DIR),$(OFED_SYMVERS))
        else
            INFO_CORE_MOD += (Not Checked!)
        endif
    else
        INFO_CORE_MOD := NOT Building IB Core Modules for $$(OFED_FULL_VER) - $$(OFED_MODS_SRC_DIR)/drivers/infiniband/core/Makefile not found
    endif
else
    INFO_CORE_MOD := NOT Building IB Core Modules for $$(OFED_FULL_VER) - ofeds/$$(OFED_FULL_VER) or ofeds/$$(OFED_VER) not found
endif
endef
