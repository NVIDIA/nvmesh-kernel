#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Writes to Journal MD
# Usage:
# rw_jmd.sh <disk_id> <journal_range> <range_entry> [<j2d> <tx_id> <tx_bmp> <ver>]

NVMEIB_EC_JMDC_BITS_J2D=32
NVMEIB_EC_JMDC_BITS_TX_ID=20
NVMEIB_EC_JMDC_BITS_TX_BMP=12
NVMEIB_EC_JMDC_BITS_VER=2
# LKJ: todo, change here the TX_BMP + add Version support + Integrate with automation

# disks.csv fields Todo: Use disks_csv_find_row_val()
DISKS_CSV_BLK_SZ_FIELD=4
DISKS_CSV_DEV_FIELD=8

# serjios.csv fields
# disk,status,num_rng,free_rng,jrnl_lba,jrnl_nlbas,db_lba,db_nlbas,n_ents_rng
SERJIOS_CSV_JRNL_LBA_FIELD=5
SERJIOS_CSV_NUM_RNG=3
SERJIOS_CSV_N_ENTS_RNG_FIELD=9

if [ $# -ne 2 ] && [ $# -ne 3 ] && [ $# -ne 6 ] && [ $# -ne 7 ]; then
    echo "$0 <disk_id> <journal_range> [[<range_entry>] [<j2d> <tx_id> <tx_bmp> [data_pattern]]]"
    exit -1
fi

disk_id="$1"
range="$2"

if [ $# -ge 3 ]; then
    entry="$3"

    if [ $entry -lt 0 ]; then
        echo "Invalid entry $entry"
    fi
else
    entry=0
fi

if [ $# -eq 6 ] || [ $# -eq 7 ]; then
    write=1
    j2d="$4"
    tx_id="$5"
    tx_bmp="$6"
    #version="$7"
    if [ $# -eq 7 ]; then
        data_pattern="$7"
    else
        data_pattern="DATADATA"
    fi
fi

write_jmd()
{
    local disk_dev="$1"
    local disk_lba="$2"
    local disk_blk_sz="$3"
    local inline="$4"
    local j2d="$5"
    local tx_id="$6"
    local tx_bmp="$7"
    #local version="$8"
    local dp="$8"
    local dp_size=${#dp}

    data_file=`mktemp`

    if [ $inline -eq 1 ]; then
        md_file=$data_file
    else
        md_file=`mktemp`
    fi

    if [ $(($disk_blk_sz % $dp_size)) -ne 0 ]; then
        echo "Data Pattern must fit evenly into Disk Block"
        exit -1
    fi

    num_dps=$(($disk_blk_sz / $dp_size))
    for i in `seq 1 $num_dps`; do
        echo -n "$dp" >> $data_file
    done
    # Disk MD format is currently little-endian so we need to do some endian-swapping.
    tx_id_bmp=$(($(($tx_bmp << $NVMEIB_EC_JMDC_BITS_TX_ID)) | $(($tx_id & $((((1 << $NVMEIB_EC_JMDC_BITS_TX_ID)) - 1))))))
    jmd_hex_str=`printf "%02x%02x%02x%02x%02x%02x%02x%02x" $(($j2d & 0xff)) $(((($j2d >> 8)) & 0xff)) $(((($j2d >> 16)) & 0xff)) $(((($j2d >> 24)) & 0xff)) $(($tx_id_bmp & 0xff)) $(((($tx_id_bmp >> 8)) & 0xff)) $(((($tx_id_bmp >> 16)) & 0xff)) $(((($tx_id_bmp >> 24)) & 0xff))`
    echo $jmd_hex_str | while read -N2 char; do printf "\x$char"; done >> $md_file
    if [ $inline -eq 1 ]; then
        #echo "nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -z $(($disk_blk_sz + 8)) -d $data_file"
        sudo nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -z $(($disk_blk_sz + 8)) -d $data_file
    else
        #echo "nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y 8 -d $data_file -M $md_file"
        sudo nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y 8 -d $data_file -M $md_file
    fi
}

read_jmd()
{
    local disk_dev="$1"
    local disk_lba="$2"
    local disk_blk_sz="$3"
    local inline="$4"
    local start_range_lba=$((jrnl_part_start_lba + $(($range * 128))))

    data_file=`mktemp`

    if [ $inline -eq 1 ]; then
        #echo "nvme read $disk_dev -s $disk_lba -z $(($disk_blk_sz + 8))"
        sudo nvme read $disk_dev -s $disk_lba -z $(($disk_blk_sz + 8)) -d $data_file
        read -r -a md_hex <<< `hexdump -s $disk_blk_sz -ve '1/1 "%02x "' $data_file`
    else
        md_file=`mktemp`
        #echo "nvme read $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y 8 -M $md_file"
        sudo nvme read $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y 8 -d $data_file -M $md_file 2> /dev/null > /dev/null
        read -r -a md_hex <<< `hexdump -ve '1/1 "%02x " ' $md_file`
    fi

    j2d_hex_str="0x${md_hex[3]}${md_hex[2]}${md_hex[1]}${md_hex[0]}"
    j2d=$(($j2d_hex_str))
    tx_id_bmp_str="0x${md_hex[7]}${md_hex[6]}${md_hex[5]}${md_hex[4]}"
    tx_id_bmp=$(($tx_id_bmp_str))
    tx_id=$(($tx_id_bmp & $((((1 << $NVMEIB_EC_JMDC_BITS_TX_ID)) - 1))))
    tx_bmp=$(($tx_id_bmp >> $NVMEIB_EC_JMDC_BITS_TX_ID))
    tx_bmp_str=`printf %04x $tx_bmp`

    echo "Range: $range Entry: $(($disk_lba - $start_range_lba)) LBA: $disk_lba = J2D: $j2d TxID: $tx_id TxBmp: $tx_bmp_str Raw: ${md_hex[*]} Data File: $data_file"
}

# Find disk in serjios.csv
serjio_disk_row=`sudo grep $disk_id /proc/nvmeibs/serjios.csv`
if [ -z $serjio_disk_row ]; then
    echo "Could not find disk $disk_id in serjios.csv"
    exit -1
fi

jrnl_part_start_lba=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_JRNL_LBA_FIELD`
jrnl_part_num_rng=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_NUM_RNG`
n_ents_rng=`echo $serjio_disk_row | cut -d, -f$SERJIOS_CSV_N_ENTS_RNG_FIELD`

if [ $range -lt 0 ]; then
    echo "Invalid range $range"
    exit -1
fi

if [ $range -ge $jrnl_part_num_rng ]; then
    echo "Invalid range $range"
    exit -1
fi

if [ $entry -ge $n_ents_rng ]; then
    echo "Invalid entry $entry > $n_ents_rng"
    exit -1
fi

# Get Disk's device name and block size
disk_dev=`grep $disk_id /proc/nvmeibs/disks.csv | cut -d',' -f$DISKS_CSV_DEV_FIELD`
disk_blk_sz=`grep $disk_id /proc/nvmeibs/disks.csv | cut -d',' -f$DISKS_CSV_BLK_SZ_FIELD`

# Determine if metadata is inline or separate
flbas=`sudo nvme id-ns $disk_dev | grep flbas | cut -d: -f2 | tr -d " "`
if [[ $(($flbas >> 4)) -eq 1 ]]; then
    inline=1
else
    inline=0
fi

ent_lba=$(($(($(($range * $n_ents_rng)) + $entry)) + $jrnl_part_start_lba))

if [ $write ]; then
    echo "Disk $disk_id - Writing JMD $j2d:$tx_id:$tx_bmp to Range $range Entry $entry (LBA: $ent_lba)"
else
    if [ $# -lt 3 ]; then
        echo "Disk $disk_id - Reading JMD from Range $range Entries 0 to $(($n_ents_rng - 1)) (LBAs: $ent_lba - $(($ent_lba + $(($n_ents_rng - 1)))))"
    else
        echo "Disk $disk_id - Reading JMD from Range $range Entry $entry (LBA: $ent_lba)"
    fi
fi

if [ $write ]; then
    write_jmd $disk_dev $ent_lba $disk_blk_sz $inline $j2d $tx_id $tx_bmp $data_pattern
elif [ $# -lt 3 ]; then
    for i in `seq 0 $(($n_ents_rng - 1))`; do
        read_jmd $disk_dev $(($ent_lba + $i)) $disk_blk_sz $inline
    done
else
    read_jmd $disk_dev $ent_lba $disk_blk_sz $inline
fi
