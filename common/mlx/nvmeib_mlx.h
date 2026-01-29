/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_MLX_H
#define NVMEIB_MLX_H

#include "nvmeib_utils.h"
#include "nvmeib_ib_driver.h"
#include "nvmeibm_trace.h"

struct nvmeibc_remote_net;
static inline int 
nvmeib_mlx_calc_riu(struct nvmeibc_remote_net *rnet, void *start, void *end)
{
	struct ib_sge *rsq_sge;
	struct nvmeib_rdma_iu *rsq_riu;
	int offset;

	NFIN;
	if (end > start) {
		offset = start - rnet->rsq;
		rsq_sge = &rnet->rsq_sge1;
		rsq_riu = &rnet->rsq_riu1;
		rsq_sge->addr = rnet->rsq_dma + offset;
		rsq_sge->length = end - start;
		rsq_riu->raddr = rnet->remote_sendq_buffer_raddr + offset;
		rnet->wrap_around = false;
	}
	else {
		rsq_sge = &rnet->rsq_sge1;
		rsq_riu = &rnet->rsq_riu1;
		offset = start - rnet->rsq;
		rsq_sge->addr = rnet->rsq_dma + offset;
		rsq_sge->length = rnet->rsqe - start;
		rsq_riu->raddr = rnet->remote_sendq_buffer_raddr + offset;
		start = rnet->rsq;
		if (end > start) {
			rsq_sge = &rnet->rsq_sge2;
			rsq_riu = &rnet->rsq_riu2;
			rsq_sge->addr = rnet->rsq_dma;
			rsq_sge->length = end - start;
			rsq_riu->raddr = rnet->remote_sendq_buffer_raddr;
			rnet->wrap_around = true;
		}
		else
			rnet->wrap_around = false;
	}
	_ND(nvmeib_mlx_calc_riu_d1, "wrap_around=@STR", rnet->wrap_around ? "true" : "false");
	NFOUT;
	return 0;
}

#if MOFED_VERSION_LT(5,1)
int nvmeib_mlx4_init(void);
void nvmeib_mlx4_cleanup(void);
#else
static inline int nvmeib_mlx4_init(void) {return 0;}
#define nvmeib_mlx4_cleanup(...)
#endif

int nvmeib_mlx5_init(bool paging_enabled);
void nvmeib_mlx5_cleanup(void);

#endif

