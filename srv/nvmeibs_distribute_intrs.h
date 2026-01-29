/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_DISTRIBUTE_INTRS_H
#define NVMEIBS_DISTRIBUTE_INTRS_H


void wait_until_distribute_interrupts_done(void);
int trigger_distribute_nvme_interrupts(void);


#endif

