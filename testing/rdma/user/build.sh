#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

SERVERS="10.0.1.51 10.0.1.53"
for s in $SERVERS; do
	ssh $s "if [ ! -d test ]; then mkdir test; fi"
	scp rdma.c Makefile rdma.sh $s:test/
done

for s in $SERVERS; do
	echo "compiling on $s"
	ssh $s "cd test; make" 
done

wait
echo "DONE"


