#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if [ `id -un` != "root" ] ; then
        echo "This script needs to run as root."
        exit 1
fi

# Remove modules
rmmod tcp_c
rmmod tcp_s

# Rebuild modules
#make clean
#make

# Insert modules
sudo insmod tcp_s.ko
sudo insmod tcp_c.ko

# List modules
lsmod | grep tcp_
