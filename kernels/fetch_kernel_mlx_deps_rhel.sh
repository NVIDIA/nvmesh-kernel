#!/usr/bin/env bash

#Note: This script must be run from a RH machine with an access to RH repo

KERNEL=${1:-`uname -r`}
KERNEL_NO_X86="${KERNEL%.x86_64}"

NVMESH_ROOT=${2:-`pwd`}
KERNELS_ROOT=${NVMESH_ROOT}/kernels

KERNEL_REPO=${3:-rhel-*-for-x86_64-baseos-source-rpms}
# Download rh source rpm
CURRENT=`pwd`

function usage() {  
  echo $0 [KERNEL_TO_FETCH] [NVMESH_ROOT]
  echo "[NVMESH_ROOT] should be set if running the script outside of nvmesh root"
  exit 2
}
case $1 in 
 -h) usage ;;
  h) usage ;;
  help) usage ;;
  ?) usage ;;
esac
[[ -d $KERNELS_ROOT ]] || usage

function finish {
  set +e
  cd $CURRENT
  [[ -d "$TMP" ]] && rm -rf $TMP
}
trap finish EXIT

set -e
cd ${KERNELS_ROOT}
TMP=$(mktemp -d -p `pwd`)
cd $TMP
sudo dnf download kernel-${KERNEL_NO_X86} --disablerepo="*" --enablerepo="${KERNEL_REPO}" --source
# we now have file named kernel-${KERNEL_NO_X86}.src.rpm
rpm2cpio ./kernel-${KERNEL_NO_X86}.src.rpm | cpio -i
tar -xf linux-${KERNEL_NO_X86}.tar.xz
KERNEL_SRCS_PATH="$(pwd)/linux-${KERNEL_NO_X86}"

# This would probably need to be extracted to seperate script that only
# Moves from kernel sources folder the needed files
FILES="drivers/infiniband/hw/mlx4/mlx4_ib.h drivers/infiniband/hw/mlx5/mlx5_ib.h drivers/infiniband/hw/mlx5/srq.h"
cd -
mkdir -p $KERNEL

for file in $FILES; do
    tgt=${KERNEL}/${file}
    tgt_dir=$(dirname ${tgt})
    [ -d "${tgt_dir}" ] || mkdir -p $tgt_dir
    mv ${KERNEL_SRCS_PATH}/$file $tgt_dir
done

echo "Done. Files added to ${KERNEL}"

