#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

devices_path=/sys/bus/pci/devices
# First we bind all orphans to nvme
for pci in $(lspci -Dd ::0108 | cut -f1 -d ' ')
do
        if [ ! -d "$devices_path/$pci/driver" ]
        then
                echo -n "$pci" > /sys/bus/pci/drivers/nvme/bind
                sleep 1 # let udev update links
        fi
done
uuid_dir="/dev/disk/by-uuid/google-local-ssds-nvme-block"
out=""
count=0
nvme_pattern="nvme[0-9]+n[0-9]+"
for symlink in "$uuid_dir"/*; do
    if [[ -L "$symlink" && "$(readlink "$symlink")" =~ ^/dev/disk/by-id/google-local-nvme-ssd-([0-9]+)$ ]]; then
        device_num="${BASH_REMATCH[1]}"
        nvme_basename=$(basename $(readlink -f $symlink))
        if ! [[ $nvme_basename =~ $nvme_pattern ]]; then
            continue
        fi
        #/sys/devices/pci0000:00/0000:00:04.0/nvme/nvme0/nvme0n1
        nvme_pci_addr=$(basename $(dirname $(dirname $(dirname $(readlink -f /sys/block/${nvme_basename})))))
        uuid=$(basename $symlink | cut -f3- -d'-')
        if [ "$count" -eq 0 ]; then
                out+="$nvme_pci_addr"";"$((device_num + 1))";"$uuid
        else
                out+=",$nvme_pci_addr"";"$((device_num + 1))";"$uuid
        fi
        ((count++))
    fi
done
echo -n $out
