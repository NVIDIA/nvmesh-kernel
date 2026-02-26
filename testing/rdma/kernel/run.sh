#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#

ARG="/sys/class/clt_infiniband_port/nvmeib-mlx4_0-1/start_test"
# ARG="/sys/class/clt_infiniband_port/nvmeib-mlx5_0-1/start_test"

if [ $# -eq 0 ]; then
		echo "No arguments supplied";
		echo "using $ARG"
else
	$ARG=$1
fi

# nvme5 controller
#GUID="fe800000000000000002c903000fe2ab"
# nvme22 controller
#GUID="fe80000000000000001e67030093210d"

# nvme6 controller
# GUID="fe800000000000000002c9030008e561"
# nvme32 controller
# GUID="fe80000000000000f452140300f543f1"
# nvme11 controller
# GUID="fe80000000000000f4521403007984e1"
# nvme14 controller
# GUID="fe80000000000000f4521403007984f1"
# nvme16 controller
# GUID="fe80000000000000e41d2d03000264b0"
# nvme11 controller
# GUID="fe80000000000000e41d2d03000264b0"
# nvme14 controller
# mlx5
# GUID="fe80000000000000e41d2d0300a350be"
# mlx4
# GUID="fe80000000000000f4521403007984f1"
# nvme16 controller
# mlx5
# GUID="fe80000000000000e41d2d03000264b0"
# nvme18 bnxt_re1 default GID
# GUID="fe80000000000000020af7fffe9836e8"
# nvme18 bnxt_re1 IP 10.10.0.18 GID
# GUID="00000000000000000000ffff0a0a0012"
# nvme45
GUID="fe80000000000000e41d2d03001f9351"

# nvme9 (mlx5_0)
# GUID="00000000000000000000ffff0a010902"

PKEY=7fff

########################################

# CMD1="host=$GUID|7fff|0,test=256|3|5"
# host parameters are:gid|pkey|service_id
# test parameters are:type|...
# type=1 rdma_write_test
#		message_size|size_of_send_q|srq|test_duration
# type=2 send_test
#		message_size|size_of_send_q|srq|number_of_messages_send
# type=3 like send_test but without interrupts
#		message_size|size_of_send_q|srq|number_of_messages_send
CMD1="host=$GUID|$PKEY|0,test=2|256|1|1|100"

echo $CMD1 > $ARG
