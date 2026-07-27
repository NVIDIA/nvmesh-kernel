#!/usr/bin/env bash

set -e
set -o xtrace

: ${BUILD_KERNEL:="false"}
PROJ_ROOT_DIR=${PWD}


## building linux kernel
#git clone --depth 1 --branch v6.6.84 git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git v6.6.84

## simplify scripting
# ln -s v6.6.84 linux

if [ "$BUILD_KERNEL" == "true" ]; then
	cd ${PROJ_ROOT_DIR}/linux

	rm -f .config

	make olddefconfig
	scripts/config \
		--enable DEBUG_INFO \
		--enable DEBUG_INFO_DWARF4 \
		--enable FRAME_POINTER \
		--enable EARLY_PRINTK \
		--enable KGDB \
		--enable KGDB_SERIAL_CONSOLE \
		--enable GDB_SHELL \
		--enable KALLSYMS \
		--enable KSELFTEST \
		--enable KUNIT \
		--enable KUNIT_DEBUGFS
	#    --enable KUNIT_TEST

	#consider to add KASAN, UBSAN, KCSAN and ...

	make olddefconfig
	make -j$(nproc)
fi

#bulding our kunit based modules
cd ${PROJ_ROOT_DIR}/nvmeibc_bio
make

sudo rm -rf ${PROJ_ROOT_DIR}/nvmeibc_bio/build

#building initramfs
mkdir -p ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/{bin,dev,etc,mnt,lib,proc,root,sbin,sys,usr/bin,usr/sbin,opt/nvmesh}
sudo mknod ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/dev/console c 5 1
sudo mknod ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/dev/null c 1 3
cp /bin/busybox ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/bin/
${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/bin/busybox --install ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/bin
cp ${PROJ_ROOT_DIR}/nvmeibc_bio/vm/init ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/
chmod +x ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/init

cp ${PROJ_ROOT_DIR}/nvmeibc_bio/nvmeibc_bio.ko ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs/opt/nvmesh/

cd ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs
find . -print0 | cpio --null -ov --format=newc > ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs.cpio

cd ${PROJ_ROOT_DIR}/

QEMU_OUTPUT_FPATH=${PROJ_ROOT_DIR}/nvmeibc_bio/build/qemu.log
QEMU_NVMESH_OUTPUT_FPATH=${PROJ_ROOT_DIR}/nvmeibc_bio/build/qemu_nvmesh.log

qemu-system-x86_64 \
  -kernel ${PROJ_ROOT_DIR}/linux/arch/x86/boot/bzImage \
  -initrd ${PROJ_ROOT_DIR}/nvmeibc_bio/build/initramfs.cpio \
  -append "console=ttyS0 root=/dev/ram rw init=/init" \
  -display none \
  -no-reboot \
  -chardev stdio,id=char0,mux=on,signal=off,logfile=${QEMU_OUTPUT_FPATH},logappend=off \
  -serial chardev:char0

sed -n '/NVMESH.KUNIT.BEGIN/, /NVMESH.KUNIT.END/p' ${QEMU_OUTPUT_FPATH} > ${QEMU_NVMESH_OUTPUT_FPATH}

${PROJ_ROOT_DIR}/linux/tools/testing/kunit/kunit.py parse ${QEMU_NVMESH_OUTPUT_FPATH}
