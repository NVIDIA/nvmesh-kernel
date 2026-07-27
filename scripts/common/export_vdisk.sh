#!/bin/bash

set -x
vdisk_name=$1
vdisk_instance=${2:-nvmesh}
echo "/dev/${vdisk_instance}/${vdisk_name},EXCELERO_VDISK/${vdisk_name},12345" | sudo tee /proc/nvmeibs/nvmeof_disks
