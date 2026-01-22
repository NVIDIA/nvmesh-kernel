#!/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

modprobe nvmeib_common_mlx4_public
modprobe nvmeib_common_mlx5_public
modprobe nvmeib_common_public
modprobe nvmeib_common
insmod pcie_atom_test.ko




