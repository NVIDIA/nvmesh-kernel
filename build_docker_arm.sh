#!/bin/bash

########################################################
# Builds NVMesh in a docker container on local machine
########################################################
# Dockerfile in docker/
# TBD:
# 	- Support different kernels
#	- Support OFED
#	- Support different distros
#	- Custom workdir

source build_common.sh
print_help() {
cat << EOF
usage:

-h      --help              prints this help

-d      --distro            linux distro (rhel7, rhel8, ...)

-p	--pkgbuild	    package build number

-b      --branch            branch name

-c      --commit-id         commit id

-g      --change-id         change id

-d      --git-describe      git describe string

-t      --target-types      target types

-m      --make-options      arguments for make command

-l	--leave-running     leave container running

-p	--rpm-path          path to copy RPM to

        --build-dir         build directory inside container

EOF
}

RSYNC_OPTS="--delete --compress --cvs-exclude --include=core --exclude=autogen/clnt --exclude=autogen/common --exclude=autogen/srv --exclude=autogen/toma --exclude=common/.trace_pp_dir --exclude=clnt/.trace_pp_dir --exclude=common_public/.trace_pp_dir --exclude=srv/.trace_pp_dir --exclude=.git --exclude=*.rpm --exclude=*.deb --ignore-errors -rlpgoDzv --checksum"

MAKE_OPTIONS="-j IM_BOTH=yes MK_RPM=yes"

LEAVE_RUNNING="false"

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
    *)
    # unknown option
    echo "Unknown option $key"
    print_help
    exit 1
    ;;
esac
shift # past argument or value
done

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

# Build docker container
echo "Building nvmesh-build-$DISTRO-arm image from docker/"
docker buildx build --platform linux/arm64/v8 -t nvmesh-build-$DISTRO-arm -f docker/Dockerfile_${DISTRO}_arm --load docker/
# Start docker container
echo "Starting container using nvmesh-build-$DISTRO-arm image"
CONT_UUID=`docker run --platform linux/arm64/v8 -dit nvmesh-build-$DISTRO-arm bash`
echo "UUID: $CONT_UUID"
# Get kernel version
KERN_VER=`docker exec $CONT_UUID bash -c "ls /lib/modules" | tr -d '\r\n'`
# Make the build dir
docker exec $CONT_UUID bash -c "mkdir -p $BUILD_DIR"
# Rsync into docker container
echo "Rsync into container $CONT_UUID:/$BUILD_DIR"
rsync -e 'docker exec -i' $RSYNC_OPTS . $CONT_UUID:/$BUILD_DIR
# Run make
MAKE_OPTIONS="$MAKE_OPTIONS COMMIT_ID=$GIT_COMMIT_ID BRANCH_NAME=$GIT_BRANCH VERSION=${GIT_DESCRIBE[0]} RELEASE=${GIT_DESCRIBE[1]} KERN_VER=$KERN_VER"
echo "Running Make - $MAKE_OPTIONS"
docker exec -t $CONT_UUID bash -c "cd $BUILD_DIR; make $MAKE_OPTIONS"
# Fetch RPM(s)
NVMESH_RPMS=$(docker exec $CONT_UUID bash -c "find /$BUILD_DIR -type f -maxdepth 1 -name '*.rpm' -o -name '*.deb' | xargs")
echo "Fetching RPM(s) $NVMESH_RPMS to $RPM_PATH"
for i in $NVMESH_RPMS; do
	docker cp $CONT_UUID:$i $RPM_PATH
done
if [ "$LEAVE_RUNNING" = "true" ]; then
	echo "Leaving Container $CONT_UUID Running"
else
	# Stopping Container
        echo "Stopping Container $CONT_UUID"
	docker container stop -t 0 $CONT_UUID
	echo "Removing Container $CONT_UUID"
	# Removing Container
	docker container rm $CONT_UUID
fi
echo "Done!"
