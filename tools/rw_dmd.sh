#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Reads/Writes Data MD
# Usage:
# rw_dmd.sh <disk_id> <lba> [<jri> <tx_id> [<edic> [<dbits>]]]

NVMEIBC_DP_EC_DMD_BITS_VERSION=2
NVMEIBC_DP_EC_DMD_BITS_EDIC=30
NVMEIBC_DP_EC_PMD_BITS_EDIC=22
NVMEIB_EC_JMDC_BITS_TX_ID=20
NVMEIBC_DP_EC_DMD_BITS_JCI=10

if [ $# -ne 2 ] && [ $# -ne 4 ] && [ $# -ne 5 ] && [ $# -ne 6 ]; then
    echo "$0 <disk_id> <lba> [<jri> <tx_id> [<edic> [<dbits>]]]"
    exit -1
fi

disk_id="$1"
lba=$(($2));	# Accept decimal and hex

if [ $# -gt 2 ]; then
    write=1
    jri="$3"
    tx_id="$4"
    if [ $# -gt 4 ]; then
        edic="$5"
        if [ $# -gt 5 ]; then
            dbits="$6"
            dmd_parity=1
        else
            dmd_parity=0
        fi
    else
        edic=0
    fi
else
    write=0
fi

function disks_csv_find_column_index() {
	local line=`head -1 /proc/nvmeibs/disks.csv`;
	local target="$1";
	echo "$line" | awk -v word="$1" -F',' '{
		for (i = 1; i <= NF; i++) {
			if ($i == word) {
				print i
				exit
			}
		}
		print 0 # Means Not found
	}'
}
function disks_csv_find_row_val() {
	i=$(disks_csv_find_column_index "$1");
	echo `grep $2 /proc/nvmeibs/disks.csv | cut -d',' -f$i`;
}
disk_dev=$(disks_csv_find_row_val "dev_name" ${disk_id});
disk_blk_sz=$(disks_csv_find_row_val "block_size" ${disk_id});
#echo "${disk_id} : ${disk_dev}, ${disk_blk_sz}";

write_dmd()
{
    local disk_dev="$1"
    local disk_lba="$2"
    local disk_blk_sz="$3"
    local inline="$4"
    local dmd_parity="$5"
    local jri="$6"
    local tx_id="$7"
    local edic="$8"
    local dbits="$9"

    # Disk MD format is currently 2 x le32 so we need to do some endian-swapping.
    if [ $dmd_parity -eq 0 ]; then
        ver_edic_dbits=$(($edic << $NVMEIBC_DP_EC_DMD_BITS_VERSION))
    else
        ver_edic_dbits=$(($(($dbits << $(($NVMEIBC_DP_EC_DMD_BITS_VERSION + $NVMEIBC_DP_EC_PMD_BITS_EDIC)))) | $(($edic << $NVMEIBC_DP_EC_DMD_BITS_VERSION))))
    fi
    tx_id_jri=$(($(($jri << $NVMEIB_EC_JMDC_BITS_TX_ID)) | $(($tx_id & $((((1 << $NVMEIB_EC_JMDC_BITS_TX_ID)) - 1))))))
    dmd_hex_str=`printf "%02x%02x%02x%02x%02x%02x%02x%02x" $(($ver_edic_dbits & 0xff)) $(((($ver_edic_dbits >> 8)) & 0xff)) $(((($ver_edic_dbits >> 16)) & 0xff)) $(((($ver_edic_dbits >> 24)) & 0xff)) $(($tx_id_jri & 0xff)) $(((($tx_id_jri >> 8)) & 0xff)) $(((($tx_id_jri >> 16)) & 0xff)) $(((($tx_id_jri >> 24)) & 0xff))`
    echo "Writing DMD: $dmd_hex_str"

    data_file=`mktemp`
    read_data_file=`mktemp`
    if [ $inline -eq 1 ]; then
        md_file=$data_file
		read_md_file=$read_data_file
    else
        md_file=`mktemp`
		read_md_file=`mktemp`;
    fi

    num_u64s_blk=$(($disk_blk_sz / 8))
    for i in `seq 1 $num_u64s_blk`; do
        echo -n 'DATADATA' >> $data_file
    done

    echo $dmd_hex_str | while read -N2 char; do printf "\x$char"; done >> $md_file

    if [ $inline -eq 1 ]; then
        cmd="sudo nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -z $(($disk_blk_sz + 8)) -d $data_file";

    else
        cmd="sudo nvme write $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y 8 -d $data_file -M $md_file";
    fi
	echo $cmd; eval $cmd;

    # Verify the MD was written
    if [ $inline -eq 1 ]; then
	     cmd="sudo nvme read $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -z $(($disk_blk_sz + 8)) -d $read_data_file";
	else
         cmd="sudo nvme read $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y 8 -d $read_data_file -M $read_md_file";
	fi
	echo $cmd; eval $cmd;
    cmd="diff $data_file $read_data_file"; echo $cmd; eval $cmd;
	cmd="diff $md_file $read_md_file";     echo $cmd; eval $cmd;
}

read_dmd()
{
    local disk_dev="$1"
    local disk_lba="$2"
    local disk_blk_sz="$3"
    local inline="$4"

    data_file=`mktemp`
    if [ $inline -eq 1 ]; then
        cmd="sudo nvme read $disk_dev -s $disk_lba -z $(($disk_blk_sz + 8)) -d $data_file";
		echo $cmd; eval $cmd;
        read -r -a md_hex <<< `hexdump -s $disk_blk_sz -ve '1/1 "%02x "' $data_file`
    else
        md_file=`mktemp`
        cmd="sudo nvme read $disk_dev -s $disk_lba -z $disk_blk_sz -c 0 -y 8 -d $data_file -M $md_file 2> /dev/null > /dev/null";
		echo $cmd; eval $cmd;
        read -r -a md_hex <<< `hexdump -ve '1/1 "%02x "' $md_file`
    fi

    ver_edic_dbits=$((0x${md_hex[3]}${md_hex[2]}${md_hex[1]}${md_hex[0]}))
    ver=$(($ver_edic_dbits & $((((1 << $NVMEIBC_DP_EC_DMD_BITS_VERSION)) - 1))))
    d_edic=$(($(($ver_edic_dbits >> $NVMEIBC_DP_EC_DMD_BITS_VERSION)) & $((((1 << $NVMEIBC_DP_EC_DMD_BITS_EDIC)) - 1))))
    d_edic_str=`printf %08x $d_edic`
    p_edic_dbits=$(($(($ver_edic_dbits >> $NVMEIBC_DP_EC_DMD_BITS_VERSION))))
    p_edic=$(($p_edic_dbits & $((((1 << $NVMEIBC_DP_EC_PMD_BITS_EDIC)) - 1))))
    p_edic_str=`printf %08x $p_edic`
    p_dbits=$(($p_edic_dbits >> $NVMEIBC_DP_EC_PMD_BITS_EDIC))
    p_dbits_str=`printf %02x $p_dbits`

    tx_id_jri=$((0x${md_hex[7]}${md_hex[6]}${md_hex[5]}${md_hex[4]}))
    tx_id=$(($tx_id_jri & $((((1 << $NVMEIB_EC_JMDC_BITS_TX_ID)) - 1))))
    jri=$(($tx_id_jri >> $NVMEIB_EC_JMDC_BITS_TX_ID))

    if [ $inline -eq 1 ]; then
    	echo -e "Read DMD: ${md_hex[*]},\n\t Data File: $data_file"
    else
		echo -e "Read DMD: ${md_hex[*]},\n\t Block File: $data_file\n\t MDu64 File: $md_file"
    fi
	#echo -e "\t Block_content : `xxd $data_file | head -1`";
	#echo -e "\t MDu64_content : `xxd $md_file   | head -1`";
    echo "Data   Fields - Version: $ver EDIC: $d_edic_str TxID: $tx_id JRI: $jri"
    echo "Parity fields - Version: $ver EDIC: $p_edic_str DBits: $p_dbits_str TxID: $tx_id JRI: $jri"
}

function echo_yellow() { echo -e "\e[0;33m$*\e[0m"; }

if [ $write -eq 1 ]; then
    echo_yellow "Disk $disk_id - Writing DMD $jri:$tx_id:$edic:$dbits to LBA: $lba"
else
    echo_yellow "Disk $disk_id - Reading DMD from LBA: `printf '%d = 0x%X\n' ${lba} ${lba}`"
fi

# Determine if metadata is inline or separate
flbas=`sudo nvme id-ns $disk_dev | grep flbas | cut -d: -f2 | tr -d " "`
if [[ $(($flbas >> 4)) -eq 1 ]]; then
    inline=1
else
    inline=0
fi

if [ $write -eq 1 ]; then
    write_dmd $disk_dev $lba $disk_blk_sz $inline $dmd_parity $jri $tx_id $edic $dbits
else
    read_dmd $disk_dev $lba $disk_blk_sz $inline
fi
