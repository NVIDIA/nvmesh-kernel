#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# This test verifies compilation of nvmesh user space utils
#function verify_user_space_utils() {
	current_dir=$(pwd);
	cd perfTest/io_stress/cmp_blocks;
	./examples/run_test.sh;
	rv=$?;
	if [ ${rv} -eq 0 ]; then
		echo "_rv=${rv} OK";
	else
		echo "_rv=${rv} Fail";
	fi;
	cd ${current_dir};
	exit ${rv};
#}
