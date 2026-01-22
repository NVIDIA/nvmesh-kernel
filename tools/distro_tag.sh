#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

distro_tag=""
if [ -z "$DISTRO_TAG" ]; then
	if [ -e /etc/redhat-release ]; then
		redhat_rel_content=`cat /etc/redhat-release`
		if [[ "$redhat_rel_content" =~ ([0-9]+).([0-9]+) ]]; then
			minor_ver=${BASH_REMATCH[2]}
			rpm_dist_tag=`rpm --eval='%{?dist}'`
			if [ -z "$minor_ver" ] || [ -z "$rpm_dist_tag" ]; then
				echo "ERROR: cannot find the distribution version"
				exit 1
			fi

			# check where to place the minor version
			IFS='.' read -r -a rpm_dist_tag_arr <<< "$rpm_dist_tag"

			if [ ${#rpm_dist_tag_arr[@]} -eq 3 ]; then
				distro_tag=".${rpm_dist_tag_arr[1]}"_"$minor_ver"
			else
				distro_tag="$rpm_dist_tag"_"$minor_ver"
			fi
		fi
	else
		#not rhel related os - ubuntu or other
		os_version_id=$(grep -oP '^VERSION_ID="*\K[^"]*' /etc/os-release)
		if [ -n "$os_version_id" ]; then
			short_version_id=$(echo $os_version_id | tr -d .)
			os_name=$(grep -oP '^NAME="*\K[^."]*' /etc/os-release)
			if [ -n "$os_name" ]; then
				# convert to lower case
				lower_os_name=$(echo "$os_name" | awk '{print tolower($0)}')
				# remove spaces
				lower_os_name=${lower_os_name//[[:blank:]]/}
				distro_tag=".$lower_os_name$short_version_id"
			fi
		fi
	fi
else
	distro_tag="$DISTRO_TAG"
fi

echo "$distro_tag"
