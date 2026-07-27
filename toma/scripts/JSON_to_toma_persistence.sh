#!/bin/bash

while (( 1 )); do
	JSON_file_name=`ls -t *.JSON | head -1`
	read -p "JSON file (default=${JSON_file_name})? " in_file_name
	if [ "_${in_file_name}" != "_" ]; then
		file_name=`ls ${in_file_name}`
		if [ `echo ${file_name} | wc -w` == 1 ]; then
			JSON_file_name=${file_name};
		else
			echo "Bad file name '${input_file_name}=${file_name}'"
			exit;
		fi;
	fi;
	if [ "_${JSON_file_name}" != "_" ]; then
		break;
	fi;
done

output_file_name=${JSON_file_name##*/}	# Just the file name (as if in the current directory)
output_file_name="./${output_file_name}`date +_%d%b%Y_%H%M%S`.persistence"
read -p "persistence output file (default=${output_file_name})? " file_name
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

cmdline="sudo ${toma_executable} --convert-to-persistence --input-file ${JSON_file_name} --output-file ${output_file_name}"
echo Running ${cmdline}
${cmdline}

