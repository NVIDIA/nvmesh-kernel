#!/bin/bash

##############################################################################
#  Copyright (C) 2015-2018 Excelero, Inc. All Rights Reserved.               #
#                                                                            #
#  This file is part of Excelero NVMesh software.                            #
#                                                                            #
#  Unauthorized copying of this file, via any medium is strictly prohibited  #
#  Proprietary and confidential                                              #
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
