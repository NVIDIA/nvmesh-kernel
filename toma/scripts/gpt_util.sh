#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

toma_executable=`ls bin/release/nvmeibt_toma`
kernel_release=`uname --kernel-release`;
toma_executable_public=`ls /opt/nvmesh/target-repo/target_*${kernel_release}/toma/bin/release/nvmeibt_toma`;
if [ "_${toma_executable_public}" != "_" ]; then
	if [ "_${toma_executable}" == "_" ]; then
		toma_executable=${toma_executable_public};
	else
		echo "	BTW, found file:       ${toma_executable_public}";
	fi
fi
read -p "TOMA executable (default=${toma_executable})? " file_name
if [ "_${file_name}" != "_" ]; then
	toma_executable=${file_name};
fi;

cmdline="sudo ${toma_executable} gpt_util $@"
echo Running ${cmdline}
${cmdline}

