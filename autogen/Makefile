# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# .a.tr is an empty target file where the corresponding a.sh file
# serves as both the prerequisite and the recipe of that target,
# with the exception of 'subdirs.tr'.

#Include common.mk
include $(NVMESH_SRC_DIR)/scripts/common.mk

# MCS target
MGMT_CM_DIR =../management_cm
MCS_SCHEME_PY = $(MGMT_CM_DIR)/clnt/process_scheme.py
MCS_SCHEME_CLNT_JSON = $(MGMT_CM_DIR)/clnt/clnt_scheme.json
MCS_SCHEME_TOMA_JSON = $(MGMT_CM_DIR)/toma_scheme.json
MCS_SCHEME_SRV_JSON = $(MGMT_CM_DIR)/srv_scheme.json
MCS_SCHEME_COMMON_JSON = $(MGMT_CM_DIR)/clnt_toma_common_scheme.json
MCS_SH_ARGS = $(MCS_SCHEME_PY) $(MCS_SCHEME_CLNT_JSON) $(MCS_SCHEME_TOMA_JSON) $(MCS_SCHEME_SRV_JSON) $(MCS_SCHEME_COMMON_JSON)

# Add new targets here

# ===================================================================
# =             End of User Defined Section                         =
# ===================================================================

ifneq ($(MAKECMDGOALS),clean)
	ifeq ($(AUTOGEN_SUBDIRS),)
           $(error Autogen subdirs not specified. \
	   Try 'make AUTOGEN_SUBDIRS='common clnt srv toma'')
	endif
endif
SUBDIRS = $(AUTOGEN_SUBDIRS) # Defined by main Makefile

all: .mcs.tr

.subdirs.tr:
#	$(info =========== Auto Generate Sub-Directories =========)
	@mkdir -p $(SUBDIRS)
	@touch .subdirs.tr

.mcs.tr: .subdirs.tr mcs.sh $(MCS_SH_ARGS)
	$(info =========== Auto Generate MCS Files ===============)
	./mcs.sh $(MCS_SH_ARGS)
	@touch .mcs.tr

clean:
#	rm all but Makefile and *.sh
	@rm -rf .*.tr */

.NOTPARALLEL:



