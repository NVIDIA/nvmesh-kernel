/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_DEFS_H
#define NVMEIBS_DEFS_H

#define N2NV(nis_dev) nis_dev->dev
#define N2IB(nis_dev) N2NV(nis_dev)->ib_dev
#define P2NV(ib_port) N2NV(ib_port->nis_dev)
#define P2IB(ib_port) N2IB(ib_port->nis_dev)
#define C2NIS(cl) (cl->ib_port->nis_dev)

#define NVMEIBS_ID_STRING "Linux NVMEIB Controller"

#endif

