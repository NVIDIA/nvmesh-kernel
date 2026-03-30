#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

########################################################
# Builds NVMesh in a docker container on local machine
########################################################
# Dockerfile in docker/
# TBD:
# 	- Support different kernels
#	- Support OFED
#	- Support different distros
#	- Custom workdir

set -e #exit on first error
source build_common.sh
print_help() {
cat << EOF
usage:

-h      --help              prints this help

-d      --distro            linux distro (rhel7, rhel8, ...)

-b      --branch            branch name

-c      --commit-id         commit id

-g      --change-id         change id

-d      --git-describe      git describe string

-t      --target-types      target types

-m      --make-options      arguments for make command

-l	--leave-running     leave container running

-p	--rpm-path          path to copy RPM to

        --build-dir         build directory inside container

        --podman            use Podman container engine instead of Docker
EOF
}

RSYNC_OPTS="--delete --compress --cvs-exclude --include=core --exclude=autogen/clnt --exclude=autogen/common --exclude=autogen/srv --exclude=autogen/toma --exclude=common/.trace_pp_dir --exclude=clnt/.trace_pp_dir --exclude=common_public/.trace_pp_dir --exclude=srv/.trace_pp_dir --exclude=.git --exclude=*.rpm --exclude=*.deb --ignore-errors -rlpgoDzv --checksum"

MAKE_OPTIONS="-j IM_BOTH=yes MK_RPM=yes"

LEAVE_RUNNING="false"

CONTAINER_TOOL="docker"

while [[ $# -gt 0 ]]
do
key="$1"

case $key in
    -h|--help)
    print_help
    exit 0
    ;;
    -d|--distro)
    DISTRO="$2"
    shift
    ;;
    -b|--branch)
    GIT_BRANCH="$2"
    shift
    ;;
    -c|--commit-id)
    GIT_COMMIT_ID="$2"
    shift
    ;;
    -g|--change-id)
    GIT_CHANGE_ID="$2"
    shift
    ;;
    -d|--git-describe)
    GIT_DESCRIBE="$2"
    shift
    ;;
    -t|--target-types)
    TARGET_TYPES="$2"
    shift
    ;;
    -m|--make-options)
    MAKE_OPTIONS="$2"
    shift
    ;;
    -l|--leave-running)
    LEAVE_RUNNING="true"
    ;;
    -p|--rpm-path)
    RPM_PATH="$2"
    shift
    ;;
    --build-dir)
    BUILD_DIR="$2"
    shift
    ;;
    --podman)
    CONTAINER_TOOL="podman"
    ;;
    *)
    # unknown option
    echo "Unknown option $key"
    print_help
    exit 1
    ;;
esac
shift # past argument or value
done

# Setup /dev/net/tun if missing.
function setup_tun_device() {
    case "$(uname -s)" in
        Linux) ;;
        *) return 0 ;;
    esac

    [ -e /dev/net/tun ] && return 0

    echo "Note: /dev/net/tun not found"

    if [ ! -d /dev/net ]; then
        sudo mkdir -p /dev/net 2>/dev/null
    fi

    if sudo modprobe tun 2>/dev/null; then
        echo "Successfully loaded tun kernel module"
        return 0
    fi

    echo "Warning: Could not load tun module"
    echo "If the build fails, run: sudo modprobe tun"
    return 0
}

if [ -z $DISTRO ]; then
        DISTRO=rhel7
fi

if [ -z $GIT_COMMIT_ID ] ; then
        GIT_COMMIT_ID=$(git_commit_id)
fi

if [ -z $GIT_CHANGE_ID ] ; then
        GIT_CHANGE_ID=$(git log -n1 --format=%b | awk '/^Change-Id: / {print $2}')
fi

if [ -z $GIT_BRANCH ] ; then
        GIT_BRANCH=$(git symbolic-ref --short --quiet HEAD) || GIT_BRANCH=$(git rev-parse HEAD)
fi

if [ -z $GIT_DESCRIBE ] ; then
        GIT_DESCRIBE=$(git describe | cut -c 2-)
fi

if [ -z $BUILD_DIR ]; then
	BUILD_DIR="/excelero"
fi

if [ -z $RPM_PATH ] ; then
	RPM_PATH=$PWD
fi

IFS='-' read -ra GIT_DESCRIBE <<< "$GIT_DESCRIBE"

setup_tun_device

trap "rm -f docker/pyproject.toml docker/poetry.lock" EXIT
# Build docker container
echo "Building nvmesh-build-$DISTRO image from docker/"
# Expose pyproject.toml and poetry.lock to the container.
cp pyproject.toml poetry.lock docker/ 2>/dev/null || true
$CONTAINER_TOOL build -t nvmesh-build-$DISTRO -f docker/Dockerfile_$DISTRO docker/
# Start docker container
echo "Starting container using nvmesh-build-$DISTRO image"
CONT_UUID=`$CONTAINER_TOOL run -dit nvmesh-build-$DISTRO bash`
echo "UUID: $CONT_UUID"
# Get kernel version
KERN_VER=`$CONTAINER_TOOL exec $CONT_UUID bash -c "ls /lib/modules | head -1" | tr -d '\r\n'`
# Make the build dir
$CONTAINER_TOOL exec $CONT_UUID bash -c "mkdir -p $BUILD_DIR"
# Rsync into docker container
echo "Rsync into container $CONT_UUID:/$BUILD_DIR"
rsync -e "$CONTAINER_TOOL exec -i" $RSYNC_OPTS . $CONT_UUID:/$BUILD_DIR
# Run make
MAKE_OPTIONS="$MAKE_OPTIONS COMMIT_ID=$GIT_COMMIT_ID BRANCH_NAME=$GIT_BRANCH VERSION=${GIT_DESCRIBE[0]} RELEASE=${GIT_DESCRIBE[1]} KERN_VER=$KERN_VER MODVERSIONS=0"
echo "Running Make - $MAKE_OPTIONS"
$CONTAINER_TOOL exec -t $CONT_UUID bash -c "cd $BUILD_DIR; make $MAKE_OPTIONS"
# Fetch RPM
NVMESH_RPMS=$($CONTAINER_TOOL exec $CONT_UUID bash -c "find /$BUILD_DIR -type f -maxdepth 1 -name '*.rpm' -o -name '*.deb' | xargs")
echo "Fetching RPM(s) $NVMESH_RPMS to $RPM_PATH"
for i in $NVMESH_RPMS; do
	$CONTAINER_TOOL cp $CONT_UUID:$i $RPM_PATH
done
if [ "$LEAVE_RUNNING" = "true" ]; then
	echo "Leaving Container $CONT_UUID Running"
else
	# Stopping Container
        echo "Stopping Container $CONT_UUID"
	$CONTAINER_TOOL container stop -t 0 $CONT_UUID
	echo "Removing Container $CONT_UUID"
	# Removing Container
	$CONTAINER_TOOL container rm $CONT_UUID
fi
echo "Done!"
