#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# The script will resassign orphans nvme drives back to inbox nvme driver.
# Orphans may be happen if toma calls unbind and exit/crash before calling bind again.
devices_path=/sys/bus/pci/devices
echo "Nvme orpahns script is running"
for pci in $(lspci -Dd ::0108 | cut -f1 -d ' ')
do
        if [ ! -d "$devices_path/$pci/driver" ]
        then
                echo "$pci has no driver, bind it back to nvme inbox driver"
                echo -n "$pci" > /sys/bus/pci/drivers/nvme/bind
        fi
done
takeover_file="/var/opt/nvmesh/.auto_takeover_drives_spec"
serials=()
# Check if the file exists
if [ -e "$takeover_file" ]; then
        while IFS=, read -r ignored_magic serial vendor model nsid; do
                serials+=("$serial")
        done < "$takeover_file"
else
        echo "No takeover file"
fi
echo $serials
for nvme_dir in /sys/class/nvme/nvme*; do
    # Check if the directory exists
    if [ -d "$nvme_dir" ]; then
        sn_file="$nvme_dir/serial"
        sn=$(cat $sn_file)
        # Check if the serial file contains "nvme_card"
        if [[ " ${serials[*]} " == " $sn " ]]; then
            device_path=$(realpath $nvme_dir/device)
            bdf=$(basename $device_path)
            echo -n "$pci" > /sys/bus/pci/drivers/nvme/unbind
            echo -n "$pci" > /sys/bus/pci/drivers/nvmeibs/bind
        fi
    fi
done
