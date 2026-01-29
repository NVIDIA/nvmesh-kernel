/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "ib_incs.h"
#include "nvmeibp_trace.h"

#include "nvmeib_public_mlx4_imp.c"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("NVMeIB Public mlx4");
MODULE_LICENSE("GPL and additional rights");

#define DEBUG_LEVEL (int)0
static int (*debug_level_f)(void);

static int __attribute__ ((__unused__)) nvmeib_public_mlx4_debug_level(void)
{
	return debug_level_f ? debug_level_f() : DEBUG_LEVEL;
}

static void nvmeib_public_mlx4_set_debug_level(int (*dlf)(void))
{
	debug_level_f = dlf;
}


static struct nvmeib_device_public_ops mlx4 = {
	.module = THIS_MODULE,
	.alloc_n_map = mlx4_alloc_n_map,
	.unmapn_n_free = mlx4_unmapn_n_free,
	.map_mr = mlx4_map_mr,

	.query_device = ib_query_device,
	.post_send_atomic = nvmeib_public_generic_post_send_atomic,
	.set_debug_level = nvmeib_public_mlx4_set_debug_level,
	.peek_cq = mlx4_peek_cq,
};

static struct nvmeib_device_public_ops mlx4_odp = {
	.module = THIS_MODULE,
	.alloc_n_map = mlx4_alloc_n_map,
	.unmapn_n_free = mlx4_unmapn_n_free,
	.map_mr = mlx4_map_mr,

	.query_device = ib_query_device,
	.post_send_atomic = nvmeib_public_generic_post_send_atomic,
	.set_debug_level = nvmeib_public_mlx4_set_debug_level,
	.peek_cq = mlx4_peek_cq,
};

#define DEV_MODNAME "mlx4_ib"

static int __init nvmeib_public_mlx4_init(void)
{
	return nvmeib_public_set_pops(DT_mlx4, DEV_MODNAME, &mlx4, &mlx4_odp);
}

static void __exit nvmeib_public_mlx4_exit(void)
{
	nvmeib_public_clear_pops(DT_mlx4);
}

module_init(nvmeib_public_mlx4_init);
module_exit(nvmeib_public_mlx4_exit);
