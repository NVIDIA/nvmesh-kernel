
# Distro Compatibility
ifneq ($(wildcard /etc/redhat-release),)
    # RHEL/CentOS/Fedora
    DISTRO_TYPE := Redhat
    DISTRO_VER := $(shell grep -o -E '[0-9]+\.[0-9]+' /etc/redhat-release)
    DISTRO_VER_MAJ := $(shell echo $(DISTRO_VER) | cut -d. -f1)
    DISTRO_VER_MIN := $(shell echo $(DISTRO_VER) | cut -d. -f2)
    DISTRO_KERNEL_VER := $(shell uname -r | cut -f1,2,3 -d.)

    ifneq ($(shell grep CentOS /etc/redhat-release),)
        DISTRO := CentOS
    else
        ifneq ($(shell grep Fedora /etc/redhat-release),)
            DISTRO := Fedora
        else
            DISTRO := RHEL
	endif
    endif

    ifeq ($(DISTRO_VER), 7.5)
        # Problem in this version - kbuild does not find symvers automatically
        INBOX_SYMVERS = /lib/modules/$(KERN_VER)/build/Module.symvers
    endif
    ifeq ($(DISTRO_VER), 7.6)
        # Problem in this version - kbuild does not find symvers automatically
        INBOX_SYMVERS = /lib/modules/$(KERN_VER)/build/Module.symvers
    endif
else
    # Use lsb_release (Ubuntu/Debian/SLES)
    #Yuri: @TODO lsb_release is not a good way to check it,
    #will not always exist on all specifically newer distros.
    #should use /etc/os-release which is a part of systemd standard,
    #also adopted by BusyBox making it ~100% valid on all systems.
    DISTRO := $(shell lsb_release -i -s)
    DISTRO_VER := $(shell lsb_release -r -s)
    DISTRO_VER_MAJ := $(shell echo $(DISTRO_VER) | cut -d. -f1)
    DISTRO_VER_MIN := $(shell echo $(DISTRO_VER) | cut -d. -f2)
    DISTRO_KERNEL_VER := $(shell uname -r | cut -f1,2,3 -d.)

    ifeq ($(DISTRO),Ubuntu)
        DISTRO_TYPE := Debian
    else
        ifeq ($(DISTRO),Debian)
            DISTRO_TYPE := Debian
        else
            DISTRO_TYPE := SLES
        endif
    endif
endif

# Check for custom Python
ifndef PYTHON_RUNTIME
    PYTHON_RUNTIME := $(shell which python3)
endif

ifndef PYTHON_WRAPPER
    PYTHON_WRAPPER := bash -c
endif

TRACE_GEN_SCRIPT := $(M)/tools/pre_processor/gen_probes2.py

ifeq ($(OFED_VER_TYPE),)
  $(error OFED_VER_TYPE cannot be empty, makefile bug)
endif

ifeq ($(OFED_VER_TYPE), none)
	BUILD_TCP := yes
endif

ifeq ($(BUILD_TCP),yes)
    $(info This build will include TCP support)
else
    $(info This build will NOT include TCP support)
endif

# Check ofed version >= maj.min
# Usage $(call ofed_ver_check,maj,min)
# Return "yes" or "no"
# Example:
# ifeq ($(call oded_ver_gte,5,0), yes)
#    $(info "Ofed version is greater or equal to 5.0")
# endif
# WARNING: Do not use indentation here!!! Or face the Makefile dark side...
define ofed_ver_check =
$(shell if [ -n "$(OFED_VER_MAJ)" ] && [ -n "$(OFED_VER_MIN)" ] ; then echo $$(( $1*256 + $2 <= $(OFED_VER_MAJ)*256 + $(OFED_VER_MIN) )) | sed 's/1/yes/' | sed 's/0/no/'; else echo no; fi)
endef

ifneq ($(filter $(CORE_UNITEST), true yes),)
  $(info Will build CORE_UNITEST)
  ccflags-y += -DCORE_UNITEST=1
else
  $(info Will not build CORE_UNITEST)
endif
