#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

USAGE_MSG="Usage: nightly_utils_package.sh -b <branch_name>"
REPO_MACHINE='10.0.1.198'
TMP_PATH='/home/nvmesh/temp_nvmesh_utils'
EXECUTABLES_MACHINE='c6.4m-o4.2-1.0.0.0-k2.6.32-696.6.3.el6_base_for_pyinstaller_executables'

got_branch=0
update_repo=0
while getopts "b:h" opt; do
  case ${opt} in
    b )
            branch=$OPTARG
            got_branch=1
      ;;
    h )
	        echo -e ${USAGE_MSG}
            exit 0
      ;;
    \? )
      echo "Invalid option: $OPTARG" 1>&2
      ;;
    : )
      echo "Invalid option: $OPTARG requires an argument" 1>&2
      ;;
  esac
done
shift $((OPTIND -1))

if [ $got_branch -eq 0 ]; then echo "Branch is required. $USAGE_MSG"; exit 1; fi

function log() {
	priority=$1
	msg=$2
	logger -p $priority -i -t "nightly_utils_package.sh" $msg
}

function log_and_exit() {
	log err $1
	exit 2
}

log info "creating utils package for branch: $branch"
output_dir="/home/compilator/RPM_output/utils_${branch}"

log info "output_dir = $output_dir, creating the dir if not exists"
[ ! -d $output_dir ] && mkdir -p $output_dir

log info "cleaning the output_dir"
rm -rf $output_dir/*

log info "running ./compile_for_mgmt.sh branch = "${branch}" output_dir = "${output_dir}""
log info "all the output will be logged to syslog under:  compile_for_mgmt.sh"
./compile_for_mgmt.sh $EXECUTABLES_MACHINE HEAD $branch $output_dir management -u

[[ $? -eq 0 ]] || log_and_exit "compile_for_mgmt.sh failed, exiting"

output_dir+='/HEAD_utils'
pkg_count=`ls $output_dir | grep -E "nvmesh-utils.*[deb|rpm]" | wc -l`

if [ $pkg_count -ne 2 ]; then
	if [ $pkg_count -eq 0 ]; then
		log err "There is no packages in $output_dir"
	else
		log err "There is a missing package, in $output_dir"
		rpm_count=`ls $output_dir | grep "nvmesh-utils.*rpm" | wc -l`
		if [ $rpm_count -eq 1 ]; then missing_pkg_type='deb'; else missing_pkg_type='rpm'; fi
		log err "The missing package is an $missing_pkg_type package, please check compile_for_mgmt.sh logs for more information"
	fi
	log_and_exit "Failed to create packages, exiting"
fi

log info "copying the packages to the repo machine: $REPO_MACHINE"
scp -r $output_dir/* nvmesh@$REPO_MACHINE:$TMP_PATH

[[ $? -eq 0 ]] || log_and_exit "failed to copy packages to: $REPO_MACHINE:$TMP_PATH, exiting"

log info "Triggering update repo script on: $REPO_MACHINE"
ssh nvmesh@${REPO_MACHINE} "sudo /usr/local/bin/update_nvmesh_utils_repo.sh"
