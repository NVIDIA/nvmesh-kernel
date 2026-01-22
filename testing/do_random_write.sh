#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if [ ! -e /tmp/babi ]; then
	dd if=/dev/urandom of=/tmp/babi bs=7M count=10
	sync
fi

(while true ; do dd if=/tmp/babi of=babi$1 bs=7M count=10 ; sync ; done) >& /tmp/do_random_write_log$1 &
