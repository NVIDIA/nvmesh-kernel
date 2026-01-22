#!/bin/bash

##############################################################################
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All Rights Reserved.  #
#                                                                            #
#  This file is part of NVMesh software.                                     #
##############################################################################

cd /sys/class/infiniband
mlx_devices=`ls`

if [ -e /sys/kernel/config/rdma_cm ]; then
cd /sys/kernel/config/rdma_cm
for x in $mlx_devices ; do
	mkdir $x
	cat $x/default_roce_mode
	rmdir $x
done
fi
