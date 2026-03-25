# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Install targets and paths for NVMesh (included from top-level Makefile).

MODULES_DIR := /lib/modules/$(KERN_VER)/extra/nvmesh
NVMESH_PREFIX_DIR := /opt/nvmesh
COMMON_REPO_DIR := $(NVMESH_PREFIX_DIR)/common-repo
CLIENT_REPO_DIR := $(NVMESH_PREFIX_DIR)/client-repo
TARGET_REPO_DIR := $(NVMESH_PREFIX_DIR)/target-repo
EXECUTABLES_DIR	:= $(NVMESH_PREFIX_DIR)/bin
DEST_TOOLS_DIR	:= $(NVMESH_PREFIX_DIR)/tools
DEST_MCS_DIR := $(CLIENT_REPO_DIR)/management_cm
DEST_COMMON_REPO_DIR := $(COMMON_REPO_DIR)/common_$(OFED_VER_STRING)_$(KERN_VER)
DEST_CLIENT_REPO_DIR := $(CLIENT_REPO_DIR)/client_$(OFED_VER_STRING)_$(KERN_VER)
DEST_TARGET_REPO_DIR := $(TARGET_REPO_DIR)/target_$(OFED_VER_STRING)_$(KERN_VER)
DEST_TOMA_DIR := $(DEST_TARGET_REPO_DIR)/toma
DEST_CONF_DIR := /etc/nvmesh
DEST_CONF_D_DIR := /etc/nvmesh/nvmesh.conf.d
VAR_OPT_DIR := /var/opt/nvmesh
MODPROBE_D_DIR := /etc/modprobe.d
DEPMOD_D_DIR := /etc/depmod.d
UDEV_RULES_D_DIR := /etc/udev/rules.d
DEST_LOG_DIR := /var/log/nvmesh
TRACE_DAEMON_DIR := /var/log/nvmesh/trace_daemon

ifeq ($(shell pgrep systemd 2> /dev/null | head -1),1)
    INSTALL_STARTUP_SCRIPTS_CMD := cp $(NVMESH_SRC_DIR)/system.d/*.service /usr/lib/systemd/system
else
    INSTALL_STARTUP_SCRIPTS_CMD := cp $(NVMESH_SRC_DIR)/init.d/nvmesh* /etc/init.d
endif

install_files:
	$(info Installing files to $(DEST_COMMON_REPO_DIR))
	@mkdir -p $(DEST_COMMON_REPO_DIR)
	@mkdir -p $(DEST_COMMON_REPO_DIR)/common
	$(info Installing scripts to $(COMMON_REPO_DIR)/common/scripts)
	@mkdir -p $(COMMON_REPO_DIR)/scripts
	@cp $(NVMESH_SRC_DIR)/scripts/common/* $(COMMON_REPO_DIR)/scripts
	$(info Installing tools to $(COMMON_REPO_DIR)/tools)
	@mkdir -p $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/dictionary.json $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/infra_shared/infra_shared.so $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_netlink.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/read_dwarf.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_memmgr_monitor.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_client_upgrade_breakdown.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/nvmesh_metrics.py $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/tools/toma_rpc/toma_rpc $(COMMON_REPO_DIR)/tools
	@cp $(NVMESH_SRC_DIR)/perfTest/io_stress/scan_locks/scan_locks_ec $(COMMON_REPO_DIR)/tools
	@mkdir -p $(COMMON_REPO_DIR)/tools/trace_daemon_2.0
	@cp $(NVMESH_SRC_DIR)/tools/trace_daemon_2.0/trace_daemon $(COMMON_REPO_DIR)/tools/trace_daemon_2.0
	@cp $(NVMESH_SRC_DIR)/tools/trace_daemon_2.0/run_trace_daemon.sh $(COMMON_REPO_DIR)/tools/trace_daemon_2.0
	@mkdir -p $(COMMON_REPO_DIR)/tools/pipe_tracer
	@cp $(NVMESH_SRC_DIR)/tools/pipe_tracer/build/bin/nvmeib_pipe_tracer $(COMMON_REPO_DIR)/tools/pipe_tracer
	@mkdir -p $(COMMON_REPO_DIR)/tools/traces_post_processor
	@cp $(NVMESH_SRC_DIR)/tools/traces_post_processor/cpager $(COMMON_REPO_DIR)/tools/traces_post_processor
	@cp $(NVMESH_SRC_DIR)/tools/traces_post_processor/pager.py $(COMMON_REPO_DIR)/tools/traces_post_processor
	$(info Installing files to $(DEST_COMMON_REPO_DIR)/common)
	@cp $(NVMESH_SRC_DIR)/common/*.ko $(DEST_COMMON_REPO_DIR)/common
	@cp $(NVMESH_SRC_DIR)/common/*.json $(DEST_COMMON_REPO_DIR)/common
	@cp $(NVMESH_SRC_DIR)/dictionaries.tar.gz $(DEST_COMMON_REPO_DIR)
	@mkdir -p $(DEST_COMMON_REPO_DIR)/common_public
	@cp -R $(NVMESH_SRC_DIR)/common_public $(DEST_COMMON_REPO_DIR)
	@mkdir -p $(DEST_COMMON_REPO_DIR)/keeper
	-@cp $(NVMESH_SRC_DIR)/keeper/*.ko $(DEST_COMMON_REPO_DIR)/keeper
	@mkdir -p $(DEST_COMMON_REPO_DIR)/softiwarp/kernel
	-@cp -R $(NVMESH_SRC_DIR)/softiwarp/kernel/*.ko $(DEST_COMMON_REPO_DIR)/softiwarp/kernel
	$(info Installing files to $(DEST_CLIENT_REPO_DIR))
	@mkdir -p $(DEST_CLIENT_REPO_DIR)/client/atom
	@cp $(NVMESH_SRC_DIR)/clnt/atom/*.ko $(DEST_CLIENT_REPO_DIR)/client/atom
	@cp $(NVMESH_SRC_DIR)/clnt/*.ko $(DEST_CLIENT_REPO_DIR)/client
	@cp $(NVMESH_SRC_DIR)/clnt/*.json $(DEST_CLIENT_REPO_DIR)/client
	@mkdir -p $(DEST_CLIENT_REPO_DIR)/symvers
	-@cp $(NVMESH_SRC_DIR)/symvers/*.ko $(DEST_CLIENT_REPO_DIR)/symvers
	$(info Installing files to $(DEST_TARGET_REPO_DIR))
	@mkdir -p $(DEST_TARGET_REPO_DIR)/target
	@cp $(NVMESH_SRC_DIR)/srv/*.ko $(DEST_TARGET_REPO_DIR)/target
	@cp $(NVMESH_SRC_DIR)/srv/*.json $(DEST_TARGET_REPO_DIR)/target
	$(info Creating symlinks in $(MODULES_DIR))
	@mkdir -p $(MODULES_DIR)
	@ln -nsf $(DEST_COMMON_REPO_DIR) $(MODULES_DIR)/common
	@ln -nsf $(DEST_CLIENT_REPO_DIR) $(MODULES_DIR)/client
	@ln -nsf $(DEST_TARGET_REPO_DIR) $(MODULES_DIR)/target
	$(info Running depmod)
	@depmod -a
	$(info Installing modprobe.d files)
	@cp $(NVMESH_SRC_DIR)/modprobe.d/nvmesh.conf $(MODPROBE_D_DIR)
	@mkdir -p $(COMMON_REPO_DIR)/modprobe.d
	@cp $(NVMESH_SRC_DIR)/modprobe.d/* $(COMMON_REPO_DIR)/modprobe.d
	$(info Installing depmod.d files)
	@cp $(NVMESH_SRC_DIR)/depmod.d/* $(DEPMOD_D_DIR)
	$(info Installing udev rules.d files)
	@cp $(NVMESH_SRC_DIR)/rules.d/* $(UDEV_RULES_D_DIR)
	$(info Installing files to $(DEST_TOMA_DIR))
	@mkdir -p $(DEST_TOMA_DIR)/bin
	-@cp -R $(NVMESH_SRC_DIR)/toma/bin/debug $(DEST_TOMA_DIR)/bin
	-@cp -R $(NVMESH_SRC_DIR)/toma/bin/delease $(DEST_TOMA_DIR)/bin
	-@cp -R $(NVMESH_SRC_DIR)/toma/bin/release $(DEST_TOMA_DIR)/bin
	@cp -R $(NVMESH_SRC_DIR)/toma/utils $(DEST_TOMA_DIR)
	$(info Installing scripts to $(CLIENT_REPO_DIR)/scripts)
	@mkdir -p $(CLIENT_REPO_DIR)/scripts
	@cp $(NVMESH_SRC_DIR)/scripts/client/* $(CLIENT_REPO_DIR)/scripts
	$(info Installing scripts to $(CLIENT_REPO_DIR)/services)
	@mkdir -p $(CLIENT_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmesh_util $(CLIENT_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmeshclient $(CLIENT_REPO_DIR)/services
	$(info Installing scripts to $(TARGET_REPO_DIR)/services)
	@mkdir -p $(TARGET_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmesh_util $(TARGET_REPO_DIR)/services
	@cp $(NVMESH_SRC_DIR)/init.d/nvmeshtarget $(TARGET_REPO_DIR)/services
	$(info Installing scripts to $(TARGET_REPO_DIR)/scripts)
	@mkdir -p $(TARGET_REPO_DIR)/scripts
	@cp -R $(NVMESH_SRC_DIR)/scripts/target/* $(TARGET_REPO_DIR)/scripts
	$(info Installing startup scripts)
	@$(INSTALL_STARTUP_SCRIPTS_CMD)
	$(info Installing binaries to /usr/bin)
	@cp -R $(NVMESH_SRC_DIR)/bin/* /usr/bin
	$(info Installing management_cm to $(DEST_MCS_DIR))
	@mkdir -p $(DEST_MCS_DIR)
	@cp -R $(NVMESH_SRC_DIR)/management_cm/* $(DEST_MCS_DIR)
	$(info Creating version files)
	@cp version $(CLIENT_REPO_DIR)/version
	@cp version $(TARGET_REPO_DIR)/version
	$(info Creating folders in $(VAR_OPT_DIR))
	$(info Installing tools to $(DEST_TOOLS_DIR))
	@mkdir -p $(DEST_TOOLS_DIR)
	@cp -R $(NVMESH_SRC_DIR)/tools/* $(DEST_TOOLS_DIR)
	$(info Creating directories in $(VAR_OPT_DIR))
	@mkdir -p $(VAR_OPT_DIR)/metadata_disk_image
	@mkdir -p $(VAR_OPT_DIR)/mcs
	@mkdir -p $(VAR_OPT_DIR)/toma
	@mkdir -p $(VAR_OPT_DIR)/block_devices_configuration
	@mkdir -p $(VAR_OPT_DIR)/block_devices_sub_vols
	@mkdir -p $(VAR_OPT_DIR)/clnt_instance_configuration
	$(info Installing tracing to $(TRACE_DAEMON_DIR))
	@mkdir -p $(TRACE_DAEMON_DIR)
	@ln -sf $(COMMON_REPO_DIR)/tools/traces_post_processor/cpager $(TRACE_DAEMON_DIR)/cpager
	@ln -sf $(COMMON_REPO_DIR)/tools/traces_post_processor/pager.py $(TRACE_DAEMON_DIR)/pager.py
	@ln -sf $(EXECUTABLES_DIR)/nvmesh_pet_messages $(TRACE_DAEMON_DIR)/nvmesh_pet_messages
	@tar zxvf $(NVMESH_SRC_DIR)/dictionaries.tar.gz -C $(TRACE_DAEMON_DIR)
	$(info Creating $(DEST_CONF_DIR))
	@mkdir -p $(DEST_CONF_DIR)
	$(info Creating $(DEST_CONF_D_DIR))
	@mkdir -p $(DEST_CONF_D_DIR)
.NOTPARALLEL:

$(DEST_CONF_DIR)/nvmesh.conf:
	$(info Creating nvmesh.conf)
	@cp $(NVMESH_SRC_DIR)/config/nvmesh.conf $(DEST_CONF_DIR)
	@read -p "Enter Management Protocol (http/https): " mgmt_prot;
	@sed -i "s/^MANAGEMENT_PROTOCOL=.*/MANAGEMENT_PROTOCOL=\"$$mgmt_prot\"/" $(DEST_CONF_DIR)/nvmesh.conf
	@read -p "Enter Management Servers (e.g. nvmesh-management:4001): " mgmt_srvs;
	@sed -i "s/^MANAGEMENT_SERVERS=.*/MANAGEMENT_SERVERS=\"$$mgmt_srvs\"/" $(DEST_CONF_DIR)/nvmesh.conf
	@read -p "Enter Kafka Servers (e.g. nvmesh-kafka:9092): " kafka_srvs;
	@sed -i "s/^KAFKA_SERVERS=.*/KAFKA_SERVERS=\"$$kafka_srvs\"/" $(DEST_CONF_DIR)/nvmesh.conf
	@read -p "Enter NICs (e.g. mlx5_0:1): " nics;
	@sed -i "s/^CONFIGURED_NICS=.*/CONFIGURED_NICS=\"$$nics\"/" $(DEST_CONF_DIR)/nvmesh.conf
.NOTPARALLEL:

$(DEST_CONF_DIR)/target_devices.conf:
	@cp $(NVMESH_SRC_DIR)/config/target_devices.conf $(DEST_CONF_DIR)

install: install_files $(DEST_CONF_DIR)/nvmesh.conf $(DEST_CONF_DIR)/target_devices.conf
.NOTPARALLEL:
