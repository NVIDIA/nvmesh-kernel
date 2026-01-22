#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -x
vdisk_name=$1
vdisk_instance=${2:-nvmesh}
echo "/dev/${vdisk_instance}/${vdisk_name},NVMESH_VDISK/${vdisk_name},12345" | sudo tee /proc/nvmeibs/nvmeof_disks
