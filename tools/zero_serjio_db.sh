#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Writes to Journal MD
# Usage:
# zero_serjio_db.sh <disk_id> [db_entry]

#NVMEIB_EC_JMDC_BITS_J2D=32
#NVMEIB_EC_JMDC_BITS_TX_ID=20
#NVMEIB_EC_JMDC_BITS_TX_BMP=10
#NVMEIB_EC_JMDC_BITS_VER=2

# disks.csv fields: Todo: Use disks_csv_find_row_val()
DISKS_CSV_BLK_SZ_FIELD=4
DISKS_CSV_DEV_FIELD=8
DISKS_CSV_MD_SZ_FIELD=9

# serjios.csv fields
# disk,status,num_rng,free_rng,jrnl_lba,jrnl_nlbas,db_lba,db_nlbas
SERJIOS_CSV_DB_LBA_FIELD=7
SERJIOS_CSV_DB_NLBAS_FIELD=8

if [ $# -lt 1 ]; then
    echo "$0 <disk_dev> [db_entry]"
fi

disk_id="$1"
db_entry="$2"

zero_lba_inline()
{
    local disk_dev="$1"
    local disk_lba="$2"
    local disk_blk_sz="$3"
    local disk_md_sz="$4"

    data_file=`mktemp`

    dd if=/dev/zero of=$data_file bs=8 count=$(($(($disk_blk_sz + $disk_md_sz)) / 8))

    echo "nvme write $disk_dev -s $disk_lba -c 0 -z $(($disk_blk_sz + $disk_md_sz)) -d $data_file"
    sudo nvme write $disk_dev -s $disk_lba -c 0 -z $(($disk_blk_sz + $disk_md_sz)) -d $data_file

    # Verify the MD was written
    read_data_file=`mktemp`
    echo "nvme read $disk_dev -s $disk_lba -c 0 -z $(($disk_blk_sz + $disk_md_sz)) -d $read_data_file"
    sudo nvme read $disk_dev -s $disk_lba -c 0 -z $(($disk_blk_sz + $disk_md_sz)) -d $read_data_file
    diff $data_file $read_data_file
}

zero_lba_sep()
{
    local disk_dev="$1"
    local disk_lba="$2"
    local disk_blk_sz="$3"
    local disk_md_sz="$4"

    data_file=`mktemp`
    md_file=`mktemp`

    dd if=/dev/zero of=$data_file bs=$disk_blk_sz count=1
    dd if=/dev/zero of=$md_file bs=$disk_md_sz count=1

    echo "nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y $disk_md_sz -d $data_file -M $md_file"
    sudo nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y $disk_md_sz -d $data_file -M $md_file
}

# Find disk in serjios.csv
serjio_disk_row=`sudo grep $disk_id /proc/nvmeibs/serjios.csv`
if [ -z $serjio_disk_row ]; then
    echo "Could not find disk $disk_id in serjios.csv"
    exit -1
fi

serjio_db_start_lba=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_DB_LBA_FIELD`
serjio_db_nlbas=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_DB_NLBAS_FIELD`

# Get Disk's device name, block size and metadata size
disk_dev=`grep $disk_id /proc/nvmeibs/disks.csv | cut -d',' -f$DISKS_CSV_DEV_FIELD`
disk_blk_sz=`grep $disk_id /proc/nvmeibs/disks.csv | cut -d',' -f$DISKS_CSV_BLK_SZ_FIELD`
disk_md_sz=`grep $disk_id /proc/nvmeibs/disks.csv | cut -d',' -f$DISKS_CSV_MD_SZ_FIELD`

# Determine if metadata is inline or seperate
flbas=`sudo nvme id-ns $disk_dev | grep flbas | cut -d: -f2`
if [[ $(($flbas >> 4)) -eq 1 ]]; then
    md_inline=1
else
    md_inline=0
fi

if [ ! -z $db_entry ]; then
    entry_lba=$(($serjio_db_start_lba + $db_entry))
    echo "Disk $disk_id - Zeroing SERJIO DB Entry $db_entry (LBA $entry_lba)"
    if [ $md_inline ]; then
        zero_lba_inline $disk_dev $entry_lba $disk_blk_sz $disk_md_sz
    else
        zero_lba_sep $disk_dev $entry_lba $disk_blk_sz $disk_md_sz
    fi
else
    end_lba=$(($serjio_db_start_lba + $serjio_db_nlbas))
    echo "Disk $disk_id - Zeroing SERJIO DB Entries 0 - $serjio_db_nlbas (LBAs $serjio_db_start_lba - $end_lba)"
    for i in seq $serjio_db_start_lba $end_lba; do
        if [ $md_inline ]; then
            zero_lba_inline $disk_dev $i $disk_blk_sz $disk_md_sz
        else
            zero_lba_sep $disk_dev $i $disk_blk_sz $disk_md_sz
        fi
    done
fi
