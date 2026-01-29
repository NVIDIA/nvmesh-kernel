#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

while (( 1 )); do
	persist_and_wire_file_name=`ls ./toma_persistence_*_raft_and_topo.0*persistence`
	if [ `echo ${persist_and_wire_file_name} | wc -w` == 0 ]; then
	 	persist_and_wire_file_name=`ls ./toma_persistence_*_raft_and_topo.0`
		if [ `echo ${persist_and_wire_file_name} | wc -w` == 0 ]; then
			persist_and_wire_file_name=`ls /var/opt/nvmesh/toma/toma_persistence_*_raft_and_topo.0`
		fi
	fi
	persist_and_wire_file_name=`echo ${persist_and_wire_file_name} | cut -f1 -d' '`
	read -p "TOMA input persistence file (default=${persist_and_wire_file_name})? " in_file_name
	if [ "_${in_file_name}" != "_" ]; then
		file_name=`ls ${in_file_name}`
		if [ `echo ${file_name} | wc -w` == 1 ]; then
			persist_and_wire_file_name=${file_name};
		else
			echo "Bad file name '${input_file_name}=${file_name}'"
			exit;
		fi;
	fi;
	if [ "_${persist_and_wire_file_name=}" != "_" ]; then
		break;
	fi;
done

output_file_name=${persist_and_wire_file_name##*/}	# Just the file name (as if in the current directory)
output_file_name="./${output_file_name}`date +_%d%b%Y_%H%M%S`.JSON"
read -p "JSON output file (default=${output_file_name})? " file_name
if [ "_${file_name}" != "_" ]; then
	output_file_name=${file_name};
fi;

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

cmdline="sudo ${toma_executable} --convert-to-json --input-file ${persist_and_wire_file_name} --output-file ${output_file_name}"
echo Running ${cmdline}
${cmdline}

