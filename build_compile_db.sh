#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if [ -n "$1" ]; then
	root_dir=$1
else
	root_dir=.
fi

if [ -n "$2" ]; then
	json_pattern=$2
else
	json_pattern=*.o.json
fi

#echo "root dir ${root_dir}"
#echo "json pattern ${json_pattern}"

output=${root_dir}/compile_commands.json
rm -f ${output}
first=1
while read line; do
	if [[ "${first}" -eq 1 ]]; then
		first=0
		echo "[" >> ${output}
	else
		echo "," >> ${output}
	fi
	grep -o "^{.*}" ${line} >> $output
done <<< "$(find ${root_dir} -type f -name "${json_pattern}")" 
if [[ "${first}" -eq 0 ]]; then
	echo "]" >> ${output}
fi
