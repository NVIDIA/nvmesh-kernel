/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DEFS_H
#define NVMEIBC_DEFS_H

#define P2NV(ib_port) ib_port->nic_dev->dev
#define P2IB(ib_port) P2NV(ib_port)->ib_dev

#endif

