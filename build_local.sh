#!/bin/bash

print_help() {
cat << EOF
usage:

-h      --help              prints this help

-b      --branch            branch name

-c      --commit-id         commit id

-g      --change-id         change id

-d      --git-describe      git describe string

-t      --target-types      target types

-m      --make-options      arguments for make command

EOF
}

while [[ $# -gt 0 ]]
do
key="$1"

case $key in
    -h|--help)
    print_help
    exit 0
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
    shift
    MAKE_OPTIONS="$@"
    break
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

if [ -z $GIT_COMMIT_ID ] ; then
        GIT_COMMIT_ID=$(git log -n1 --format=%h)
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

if [ -z $TARGET_TYPES ] ; then
        TARGET_TYPES="IM_SERVER=yes IM_CLIENT=yes"
fi

IFS='-' read -ra GIT_DESCRIBE <<< "$GIT_DESCRIBE"

IB_SRC=../ofa_kernel-2.3
IB_H_DIR=$IB_SRC/include
IB_D_DIR=$IB_SRC/drivers

for tt in $TARGET_TYPES; do
	echo "building " $tt
	make $1 $MAKE_OPTIONS all IB_H_DIR=$IB_H_DIR IB_D_DIR=$IB_D_DIR $tt COMMIT_ID=$GIT_COMMIT_ID BRANCH_NAME=$GIT_BRANCH VERSION=${GIT_DESCRIBE[0]} RELEASE=${GIT_DESCRIBE[1]}
done
