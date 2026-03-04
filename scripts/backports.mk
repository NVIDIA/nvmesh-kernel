# Set the SHELL variable to bash (fix ubuntu issues)
SHELL:=/bin/bash

# Uncomment the lines below to debug the grep utility functions below
GREP_DEBUG:=1

ifeq ($(GREP_DEBUG),1)
  ifeq ($(GREP_DEBUG_LOGFILE),)
    GREP_DEBUG_LOGFILE:=/tmp/backports_mk_$(strip $(shell date +%s)).log
    $(shell touch $(GREP_DEBUG_LOGFILE))
    export GREP_DEBUG_LOGFILE
  endif
endif

KERN_SYMVERS=$(KSRC1)/Module.symvers

ifneq ($(OFED_SRC_DIR),)
  INC_RDMA=$(OFED_SRC_DIR)
  INC_RDMA_DRV=$(OFED_SRC_DIR)
  RDMA_SYMVERS=$(OFED_SYMVERS)
else
  INC_RDMA=$(KSRC1)
  INC_RDMA_DRV=$(KERN_FILES_PATH)
  RDMA_SYMVERS=$(KERN_SYMVERS)
endif

ARCH := $(shell uname -m | sed -e s/i.86/x86/ \
                                  -e s/x86_64/x86/ \
                                  -e s/sun4u/sparc64/ \
                                  -e s/arm.*/arm/ -e s/sa110/arm/ \
                                  -e s/s390x/s390/ -e s/parisc64/parisc/ \
                                  -e s/ppc.*/powerpc/ -e s/mips.*/mips/ \
                                  -e s/sh[234].*/sh/ -e s/aarch64.*/arm64/ )

# NOTE: Backport cflags have been moved to script compute_backports.sh to
# speed up build time.

# The script caches stripped file contents so repeated checks on the same
# header (e.g. ib_verbs.h) only run sed once.
# This variable uses deferred expansion (=) so the script only runs when
# $(backports_cflags) is actually referenced in a recipe.
backports_cflags = $(shell $(SCRIPTS_DIR)/compute_backports.sh \
	"$(KSRC1)" "$(INC_RDMA)" "$(INC_RDMA_DRV)" "$(KERN_SYMVERS)" "$(RDMA_SYMVERS)" "$(ARCH)" \
	"$(GREP_DEBUG)" "$(GREP_DEBUG_LOGFILE)")
