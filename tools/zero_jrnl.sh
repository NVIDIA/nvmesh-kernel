#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Writes Zeroes to Journal MD
# Usage:
# zero_jrnl.sh <disk_id> [<start_rng> <end_rng>] [-y]

# disks.csv fields: Todo: Use disks_csv_find_row_val()
DISKS_CSV_BLK_SZ_FIELD=4
DISKS_CSV_DEV_FIELD=8

# serjios.csv fields
# disk,status,num_rng,free_rng,jrnl_lba,jrnl_nlbas,db_lba,db_nlbas,n_ents_rng
SERJIOS_CSV_JRNL_LBA_FIELD=5
SERJIOS_CSV_NUM_RNG=3
SERJIOS_CSV_N_ENTS_RNG_FIELD=9

if [ $# -ne 1 ] && [ $# -ne 2 ] && [ $# -ne 3 ] && [ $# -ne 4 ] ; then
    echo "$0 <disk_id> [<start_rng> <end rng>] [-y]"
    exit -1
fi

disk_id="$1"

if [ $# -eq 2 ] && [ "$2" == "-y" ] ; then
    auto_confirm=1
elif [ $# -eq 4 ] && [ "$4" == "-y" ] ; then
    auto_confirm=1
else
    auto_confirm=0
fi

# Find disk in serjios.csv
serjio_disk_row=`sudo grep $disk_id /proc/nvmeibs/serjios.csv`
if [ -z $serjio_disk_row ]; then
    echo "Could not find disk $disk_id in serjios.csv"
    exit -1
fi

jrnl_part_start_lba=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_JRNL_LBA_FIELD`
jrnl_part_num_rng=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_NUM_RNG`
n_ents_rng=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_N_ENTS_RNG_FIELD`

if [ $# -eq 1 ] || [ $# -eq 2 ]; then
    start_rng=0
    end_rng=$(($jrnl_part_num_rng - 1))
else
    start_rng=$2
    end_rng=$3
fi

num_rng=$(($(($end_rng - $start_rng)) + 1))

# Get Disk's device name and block size
disk_dev=`grep $disk_id /proc/nvmeibs/disks.csv | cut -d',' -f$DISKS_CSV_DEV_FIELD`
disk_blk_sz=`grep $disk_id /proc/nvmeibs/disks.csv | cut -d',' -f$DISKS_CSV_BLK_SZ_FIELD`

# Determine if metadata is inline or separate
#flbas=`sudo nvme id-ns $disk_dev | grep flbas | cut -d: -f2 | tr -d " "`
#if [[ $(($flbas >> 4)) -eq 1 ]]; then
#    inline=1
#else
#    inline=0
#fi

start_lba=$(($jrnl_part_start_lba + $(($start_rng * $n_ents_rng))))
num_lba=$(($num_rng * $n_ents_rng))
end_lba=$(($(($start_lba + $num_lba)) - 1))

echo "Disk $disk_id - Zeroing $num_rng Journal Ranges [$start_rng - $end_rng] (LBA: [$start_lba, $end_lba])"
if [ $auto_confirm -eq 0 ]; then
    read -p "Enter YES to continue: " confirm
fi

if [ $auto_confirm -eq 1 ] || [ $confirm == "YES" ]; then

    remainder=$num_lba
    buf_size=32768
    start_addr=$start_lba

    while [ $remainder -gt $buf_size ]; do
        echo nvme write-zeroes $disk_dev -s $start_addr -c $buf_size
        sudo nvme write-zeroes $disk_dev -s $start_addr -c $buf_size
        remainder=$(($remainder-$buf_size))
        start_addr=$(($start_addr+$buf_size))
    done

    if [ $remainder -gt 0 ]; then
        echo nvme write-zeroes $disk_dev -s $start_addr -c $remainder
        sudo nvme write-zeroes $disk_dev -s $start_addr -c $remainder
    fi

else
    echo "CANCELLED"
fi
