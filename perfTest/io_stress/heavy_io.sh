#!/bin/bash
# Written by Daniel, version 0.02

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

VOLUME=$1
if [ -z $VOLUME ]; then
    echo "Error! Unknown volume!"
    echo "Usage ./heavy_io.sh <volume> <offset[bytes]> <length[bytes]>"
    exit 11
fi

OFFSET_BLOCKS=$2
if [ -z $OFFSET_BLOCKS ]; then
    OFFSET_BLOCKS=0
fi

LENGTH_BLOCKS=$3
if [ -z $LENGTH_BLOCKS ]; then
    LENGTH_BLOCKS=409600000
fi

# --------------------------------------------------- Enable debug and params
sudo bash -c 'echo 2 > /sys/module/nvmeibc/parameters/debug_level'
sudo bash -c 'echo -n "#$VOLUME|clear_io_stats" > /proc/nvmeibc/cli/cli'
VERSION="$(cat /proc/nvmeibc/version)"
echo "Unitest ver 0.17: git=$VERSION"

# --------------------------------------------------- TRIM thrads
single_trim_thread () {
    for i in {0..4}; do
	sudo blkdiscard -v -o $OFFSET_BLOCKS -l $LENGTH_BLOCKS /dev/nvmesh/$VOLUME
    done
}

pids=""
echo "Trimming: Threads=8 , start=$OFFSET_BLOCKS, len=$LENGTH_BLOCKS vol=/dev/nvmesh/$VOLUME"
for item in {0..8}; do
    single_trim_thread &
    pids+="$! "
done

# --------------------------------------------------- RW for contention
TIME=30 		#run time 0 - is unlimited, other value in sec.
RW_RAND=50		#% of random reads/writes
THREADS=4096		#number of threads
echo "Running R/W: Threads=$THREADS , time=%TIME[sec] vol=/dev/nvmesh/$VOLUME"
#-v or -c for verification, -c is panic on the first error
#-F preformating the device with zeros
#-X trim the device before
# Todo: Use -o $OFFSET_BLOCKS -l $LENGTH_BLOCKS to bound R/W to area's of trim
sudo ./btest -t $TIME -T $THREADS -D -B 300000 R $RW_RAND /dev/nvmesh/$VOLUME

# ---------------------------------------------------
echo "Waiting for completion of TRIMs"
for pid in $pids; do
    wait $pid
    if [ $? -eq 0 ]; then
        echo "SUCCESS - Job $pid exited with a status of $?"
    else
        echo "FAILED - Job $pid exited with a status of $?"
    fi
done
echo "------------------------------------Done! displaying statistics: $VOLUME"

# ---------------------------------------------------
cat /proc/nvmeibc/volumes/$VOLUME/iostats
echo "------------------------------------Done!"
# --------------------------------------------------- Trim using mkfs
sudo mkfs -t ext4 /dev/nvmesh/$VOLUME
