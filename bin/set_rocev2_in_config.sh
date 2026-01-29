#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if [ ! -e /sys/module/mlx4_core/parameters/roce_mode ]; then
	exit
fi

ROCE_MODE=`cat /sys/module/mlx4_core/parameters/roce_mode`
if [[ $ROCE_MODE != 2 ]]; then
	exit
fi

if [ -e /sys/class/infiniband ]; then
cd /sys/class/infiniband
mlx_devices=`ls`

if [ -e /sys/kernel/config/rdma_cm ]; then
cd /sys/kernel/config/rdma_cm
for x in $mlx_devices ; do
	mkdir $x
	echo "RoCE V2" > $x/default_roce_mode
	rmdir $x
done
fi

fi
