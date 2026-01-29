#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Written by Daniel, version 0.02

VOLUME=$1
if [ -z $VOLUME ]; then
    echo "Error! Unknown volume!"
    echo "Usage ./correctness.sh <volume>"
    exit 11
fi

if [ `id -un` != "root" ] ; then
        echo "This script needs to run as root."
        exit 1
fi

PARENT_DIR=/dev/nvmesh
MOUNT_DIR="./res/$VOLUME""_mnt"

fio=fio
if [ -e /usr/local/bin/fio ] ; then
    fio=/usr/local/bin/fio
fi
# --------------------------------------------------- Enable debug and params
sudo bash -c 'echo 2 > /sys/module/nvmeibc/parameters/debug_level'
VERSION="$(cat /proc/nvmeibc/version)"
echo "Unitest: git=$VERSION"

# --------------------------------------------------- Create a simple text file
create_verify_simple_file() {
    fdisk -l $PARENT_DIR/$VOLUME
    echo "Mounting $PARENT_DIR/$VOLUME to $MOUNT_DIR"
    CURDIR=$(pwd)
    mkdir -p $MOUNT_DIR
    mount $PARENT_DIR/$VOLUME $MOUNT_DIR
    cd $MOUNT_DIR
    mkdir -p test_dir
    touch test_file.txt
    WRITE="HelloWorld"
    echo $WRITE > test_file.txt
    READ=$(<test_file.txt)
    if [ "$READ" == "$WRITE" ]; then
        echo "unitest OK"
    else
        echo "unitest FAIL"
    fi
    cd $CURDIR
    echo "Unmounting and deleting $MOUNT_DIR"
    umount $MOUNT_DIR
    rm -rf $MOUNT_DIR
}

# ---------------------------------------------------
volume_warmup() { # Fill all the volume (all disks) with one big file and delete it
    echo "Filling/Cleaning all the volume: $PARENT_DIR/$VOLUME"
    mount $PARENT_DIR/$VOLUME /mnt
    dd if=/dev/zero of=/mnt/tmp.dat bs=1G
    rm -r tmp.dat  # remove the file whihc occupies the entire disk
    #umount /mnt
}
volume_warmup2() {
	volnames=$1
	runtime=$2
	./do_fio "$volnames" 8 12 0 4k $runtime;
}

volume_io_through_bdev() { # Do IO directly to the volume, without file system
    sudo fio --name=babi --output-format=json --size=16G --rw=read  --numjobs=1 --bs=64k --ioengine=posixaio --direct=1 --filename=$PARENT_DIR/$VOLUME > out_bdev.json
}

volume_io_to_disk_backdoor() {
    # Find the first Intel disk named 'CVM...' that the volume uses, find its backdoor number /dev/nvmes?n1 and do fio to it (raw fio to disk)
    cat /proc/nvmeibc/volumes/$PARENT_DIR/$VOLUME/status > stats1.txt
    awk '{for(i=1;i<=NF;i++){if($i~/^CVM/){print $i}}}' stat.txt > stat2.txt
    grep -m 1 '.*' stat3.txt
    DISK_NAME=$(<stat3.txt) # Todo: Disk name is XXXX.Y, we need the XXXX only, remove this with grep
    DISK_INDX=$(grep $DISK_NAME /proc/nvmeibs/smart* | grep -o 'smart.' | grep -o '[0-9]')
    sudo fio --name=babi --output-format=json --size=16G --rw=read  --numjobs=1 --bs=64k --ioengine=posixaio --direct=1 --filename=/dev/nvme$DISK_INDXn1 > out_raw.json
}

# ---------------------------------------------------
clear_volume_nvmeib_io_stats() {
    IOCTL_TO="/sys/kernel/config/nvmeibc/operation"
    IOCTL_TO="/proc/nvmeibc/cli/cli"
    echo -n "$VOLUME|clear_io_stats" > $IOCTL_TO
}

measure_performance() {
    echo "Mounting $PARENT_DIR/$VOLUME to $MOUNT_DIR"
    CURDIR=$(pwd)
    mkdir -p $MOUNT_DIR
    mount $PARENT_DIR/$VOLUME $MOUNT_DIR
    cd $MOUNT_DIR
    mkdir -p test_dir

    # egnines: % fio --enghelp
    # ?-ioengine=libaio/sync/posixaio/mmap
    #common=" --output-format=json --group_reporting=1 --direct=1 --invalidate=1"
    # explanation of fio output: https://tobert.github.io/post/2014-04-17-fio-output-explained.html
    HOSTNAME=`hostname -s`
    TSTAMP=`date +%Y%m%d%H%M%S`
    FIO_FORMAT=json # can use =normal
    FIO_RESULT_FULL_PATH=$CURDIR/pt_fio-$HOSTNAME-$TSTAMP.$FIO_FORMAT
    EXC_RESULT_FULL_PATH=$CURDIR/pt_exc-$HOSTNAME-$TSTAMP.txt
    echo "Writing results to $CURDIR/."
    echo "FIO output: $FIO_RESULT_FULL_PATH, EXC:$EXC_RESULT_FULL_PATH"
    echo "" > $FIO_RESULT_FULL_PATH
    BLK_SIZES_IN_KB=(1 4 8 16 32 64 128 256 512 1024)
    #dmesg --clear
    clear_volume_nvmeib_io_stats

    for DIRECT in 1 0; do
	for ENGINE in sync posixaio libaio; do
	    for BLKSIZE in ${BLK_SIZES_IN_KB[@]}; do
		echo Start `date`: $ENGINE $BLKSIZE write
		clear_volume_nvmeib_io_stats
		fio $common --name=$HOSTNAME-$ENGINE-$BLKSIZE-write --output-format=$FIO_FORMAT --size=16m --rw=write  --numjobs=1 --bs=${BLKSIZE}k --ioengine=$ENGINE --direct=$DIRECT >> $FIO_RESULT_FULL_PATH
		echo "***write D=$DIRECT E=$ENGINE B=$BLKSIZE" >> $EXC_RESULT_FULL_PATH
		cat /proc/nvmeibc/volumes/$VOLUME/iostats >> $EXC_RESULT_FULL_PATH

		echo Start `date`: $ENGINE $BLKSIZE read
		clear_volume_nvmeib_io_stats
		fio $common --name=$HOSTNAME-$ENGINE-$BLKSIZE-read --output-format=$FIO_FORMAT --size=16m --rw=read   --numjobs=1 --bs=${BLKSIZE}k --ioengine=$ENGINE --direct=$DIRECT >> $FIO_RESULT_FULL_PATH
		echo "***read D=$DIRECT E=$ENGINE B=$BLKSIZE" >> $EXC_RESULT_FULL_PATH
		cat /proc/nvmeibc/volumes/$VOLUME/iostats >> $EXC_RESULT_FULL_PATH

		echo Start `date`: $ENGINE $BLKSIZE randrw
		clear_volume_nvmeib_io_stats
		fio $common --name=$HOSTNAME-$ENGINE-$BLKSIZE-randrw --output-format=$FIO_FORMAT --size=2m  --rw=randrw --numjobs=8 --bs=${BLKSIZE}k --ioengine=$ENGINE --direct=$DIRECT --rwmixread=80 >> $FIO_RESULT_FULL_PATH
		echo "***randrw D=$DIRECT E=$ENGINE B=$BLKSIZE" >> $EXC_RESULT_FULL_PATH
		cat /proc/nvmeibc/volumes/$VOLUME/iostats >> $EXC_RESULT_FULL_PATH

		rm -f *randrw.* *read.* *write.*
	    done # BLKSIZE
	done # ENGINE
    done
    echo End `date`

    cd $CURDIR
    echo "Unmounting and deleting $MOUNT_DIR"
    umount $MOUNT_DIR
    rm -rf $MOUNT_DIR
    chown daniel: pt_*.*
}

measure_performance_multi_slice_ec() {
    VOLUME="ec_test_ms"
    profiler_path="../clnt/block/datapath_utils_generic/profiling/nvmesh_profiling.py"
    for slice in 1 4 8; do
	    echo "$slice" ;
	    NVMESH_vol_detach -a;
	    echo $slice > /sys/module/nvmeibc/parameters/nvmeibc_ec_max_slices_for_ms_io ;
	    NVMESH_vol_attach $VOLUME;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    ./do_fio $VOLUME 32 32 0 16k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    cat tmp.out | grep -E "write:| lat (usec): min|numjobs" > $slice_slice_16k.sum ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 16.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
	    ./do_fio $VOLUME 16 16 0 64k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    cat tmp.out | grep -E "write:| lat (usec): min|numjobs" > $slice_slice_64k.sum ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 64.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
	    ./do_fio $VOLUME 8 16 0 128k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    cat tmp.out | grep -E "write:| lat (usec): min|numjobs" > $slice_slice_128k.sum ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 128.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
	    ./do_fio $VOLUME 8 8 0 256k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    cat tmp.out | grep -E "write:| lat \(usec\): min|numjobs" > $slice_slice_256k.sum ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 256.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
    done

    for slice in 1 4 8; do
	    echo "$slice" ;
	    NVMESH_vol_detach -a;
	    echo $slice > /sys/module/nvmeibc/parameters/nvmeibc_ec_max_slices_for_ms_io ;
	    NVMESH_vol_attach $VOLUME;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    ./do_fio_serial $VOLUME 16 16 0 16k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    grep -E "write:| lat \(msec\): min| lat \(usec\): min|numjobs" tmp.out > slice_16k.sum_$slice ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 16.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
	    ./do_fio_serial $VOLUME 8 8 0 64k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    grep -E "write:| lat \(msec\): min| lat \(usec\): min|numjobs" tmp.out > slice_64k.sum_$slice ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 64.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
	    ./do_fio_serial $VOLUME 4 8 0 128k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    grep -E "write:| lat \(msec\): min| lat \(usec\): min|numjobs" tmp.out > slice_128k.sum_$slice ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 128.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
	    ./do_fio_serial $VOLUME 4 4 0 256k 60 >tmp.out 2>&1 ;
	    sleep 2 ;
	    grep -E "write:| lat \(msec\): min| lat \(usec\): min|numjobs" tmp.out > slice_256k.sum_$slice ;
	    sleep 2 ;
	    $profiler_path/nvmesh_profiling.py -v $VOLUME > 256.prof_$slice ;
	    echo "#|clear_profilers" > /proc/nvmeibc/cli/cli ;
	    sleep 2 ;
	    echo "DONE SLICE $slice" ;
    done
}

# ---------------------------------------------------
#Todo: use sudo file -sL  to detect if ext4 already exists and if so skip mkfs
mkfs -t ext4 $PARENT_DIR/$VOLUME
create_verify_simple_file

# sudo mkfs -t xfs -b size=4096 -f $PARENT_DIR/$VOLUME
#sudo mkfs -t xfs -f -K /dev/nvmesh/r2
#sudo fio --name=babi-write --output-format=json --size=16G --rw=read  --numjobs=1 --bs=64k --ioengine=posixaio --direct=1 --filename=$PARENT_DIR/$VOLUME > out.json

#measure_performance
measure_performance_multi_slice_ec
echo "------------------------------------Done!"

