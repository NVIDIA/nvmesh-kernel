/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_GLOBALS_H
#define NVMEIBC_BLOCK_GLOBALS_H

#include "module/instance/nvmeibc_cinst_params.h"
#include "common/compat/kr_incs_percpu.h"

struct t_block_clnt_globals * __get_from_params_blok_globals_container(const struct nvmeibc_cinst_params_blk *p)
{
	return ((struct t_block_clnt_globals *)(*(p)->_private));
}

#include "block/datapath_ec/nvmeibc_block_dp_ec_gf.h"
int MAX_NUM_ACTIVE_CPUS = 0;
int t_blok_clnt_globals_init(const struct nvmeibc_cinst_params_blk *p)
{
	extern void nvmeibc_block_dp_ec_dmd_read_mod_wr(void*, u64);
	extern void init_roles_pair_compression_tables(void);
	extern int nvmeibc_operation_all_create(void);
	int gf_val, rv = -ENOMEM;
	const bool is_first_instace = nvmeibc_cinst_is_first_blok_instance(p);

	if (is_first_instace) {						// First instance only initializations
		if (1) {								// Todo: Move to common?
			int cpu, last_cpu = 0;
			for_each_online_cpu(cpu) if (cpu > last_cpu) last_cpu = cpu;
			MAX_NUM_ACTIVE_CPUS = last_cpu + 1;
		}
		nvmeibc_operation_all_create();
		init_roles_pair_compression_tables();   // init compression table
		gf_val = __gf_choose_functions(NVMEIBC_GF_AUTO_INIT);
		BUILD_BUG_ON(N_MAX_RAID_SLICE_LEN > (GF_MAX_D+GF_MAX_P));	// Slices is too big for implemented gf calculations

		if (nvmeib_allocate_xsave_bufs() != 0)
			goto _out;
		/* Todo: Here do measurements of crc functions */
		__verify_dirtybits_union_bits();
		__verify_u32_lock_in_blockset_bits();
		__verify_u64_ec_dmd_bits();
		__verify_u64_ec_jmd_bits();
		nvmeib_set_block_dp_ec_funcs(nvmeibc_block_dp_ec_dmd_read_mod_wr);
	} else {
		gf_val = __gf_choose_functions(NVMEIBC_GF_DISPLAY_CURRENT);
	}
	_NT(trace_1_c_block_init, "gf=@STR(@INT)", nvmeibc_gf_optimization_to_string(gf_val), gf_val); (void)gf_val;

	{	// Per instance block globals
		extern struct nvmeibc_os_apis_container * nvmeibc_os_api_layer_init(const struct nvmeibc_cinst_params_blk *p);
		struct t_block_clnt_globals *b = (void*)kzalloc(sizeof(*b), GFP_KERNEL);
		if (!b)
			goto _out;
		INIT_LIST_HEAD(&b->block_devices);
		spin_lock_init(&b->block_devices_sl);
		b->watchdog.thread = NULL;
		b->watchdog.thread_counter = 0;
		b->cpu_masks = nvmeibc_b_cp_cpu_masks_create();
		if (!b->cpu_masks) {
			kfree(b);
			goto _out;
		}
		b->osc = nvmeibc_os_api_layer_init(p);
		if (!b->osc) {
			nvmeibc_b_cp_cpu_masks_destroy(b->cpu_masks);
			kfree(b);
			goto _out;
		}
		(*p->_private) = b;
		nvmeibc_cinst_get_blok_p(b) = p;
		rv = 0;
	}
_out:
	if (unlikely(rv)) {
		_NE(t_01_bglinit, DMESG_PREFIX() ": failure rv=@RV", rv);
	}
	return rv;
}

void t_blok_clnt_globals_destroy(const struct nvmeibc_cinst_params_blk *p)
{
	const bool is_first_instace = nvmeibc_cinst_is_first_blok_instance(p);
	struct t_block_clnt_globals *b =__get_from_params_blok_globals_container(p);
	extern void nvmeibc_os_api_layer_destroy(const struct nvmeibc_cinst_params_blk *p);
	if (!b)
		return;														// Destroy on creation error
	if (is_first_instace) {
		extern void nvmeibc_operation_all_destroy(void);
		nvmeib_set_block_dp_ec_funcs(NULL);
		nvmeib_free_xsave_bufs();
		nvmeibc_trs_hash_verify_empty_unsafe();
		nvmeibc_operation_all_destroy();
	}
	nvmeibc_os_api_layer_destroy(p);
	nvmeibc_b_cp_cpu_masks_destroy(b->cpu_masks);
	kfree(b);
	(*(p)->_private) = NULL;
	//_NT(trace_dp_io_generic_cmds_nvmeibc_block_layer_destroy, "block_dp_ec_funcs / JAM, correctly removed");
}

#endif // NVMEIBC_BLOCK_GLOBALS_H
