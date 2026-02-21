/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_scenario_ec_serjio.h"
#include "block/datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.h"
#include "block/unitest/server/nvmeibs_main_sim.h"
#include "kr_incs.h"
#include "nvmeibc_disk_hooks.h"
#include "nvmeibc_jam.h"
#include "nvmeibc_icore_ops.h"
#include "nvmeibc_simu_disk.h"
#include "nvmesh_sim.h"
#include "server/nvmeibs_serjio_sim_access.h"
#include "uni_framework/cond_wait_algorithms.h"
#include "uni_framework/simu_test.h"
#include "uni_random.h"
#include "uni_scenario_common.h"
#include "uni_scenario_tx_history_ec.h"
#include "uni_scenarios/uni_enumerators.h"

/****************************************************************************/
/*                            GLOBAL VARIABLES                              */
/****************************************************************************/

static const struct volume_segment_index SERJIO_UNI_VSI = {0, 0, 0, 3};

/****************************************************************************/
/*                            DATA STRUCTURES                               */
/****************************************************************************/

/**
 * Datastructure used to emulate the way htr issues gen commands.
 * Normally, htr gen commands pass through datapath object callbacks.
 * This is sometimes not what we want in simulator, since we may want our own callbacks.
 * This datastructure and related functions are used to issue htr commands in a controlled way.
 * @todo: For nove used only in serjio, but consider moving to a separate infa.
 */
struct sim_htr_gen_cmd {
	/** Holds iocmd, params and response */
	struct nvmeibc_disk_gen_cmd gen_cmd;
	/** Holds block command */
	struct nvmeibc_disk_io_command iocmd;
	/** Holds operation */
	struct nvmeibc_block_command cmd;
	/** Holds the datapath pointer */
	struct operation o;
	/** Related client's uuid */
	const uuid_be *cuuid;
	/** Buffer used by the command to store data */
	void *buf;
	/** Command buffer size */
	size_t buf_size;
};

struct serjio_uni_ctx {
	/** Env shared between tests */
	struct test_context env;
	/** Basic tool used to make io */
	u32 slice_size;
	u32 mem_size;
	u8 *mem;
	u64 magic_pattern;
};

struct serjio_uni_read_jmdc_ctx {
	/** Parent context */
	struct serjio_uni_ctx super;
	/** Inspected segment id */
	u32 si;
	/** JRI test operates on */
	u32 jri;
	/** Allocated jentry ids (with respect to alloc_bmp) */
	u32 jentries[NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE];
	/** Allocated jentries bitmap */
	u64 alloc_bmp;
	/** Abandoned jentries bitmap */
	u64 abandoned_bmp;
	/** Ptrs pointing to actual ram location of md for each corresponding entry in.jentries*/
	struct block_inject_ptrs bptrs[NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE];
	/** Used to send read_jmdc command `htr way` */
	struct sim_htr_gen_cmd *hgc;
};

struct serjio_uni_gen_wrap_ctx {
	/** Parent context */
	struct serjio_uni_ctx super;
	/** Inspected segment id */
	u32 si;
	/** DLBA test operates on */
	u32 dlba;
	/** JRI test operates on */
	u32 jri;
	/** Jentry test operates on */
	u32 jentry;
	/** Used to drain jam free list */
	int *io2sgmnt2entry;
};

/****************************************************************************/
/*                            UTILITY FUNCTIONS                             */
/****************************************************************************/


/**
 * Allocate resources for mock htr gen command
 */
struct sim_htr_gen_cmd *init_sim_htr_gen_cmd(struct test_context env, size_t jmdc_sz, size_t ent_md_sz) {
	struct sim_htr_gen_cmd *hgc = sim_kzalloc(sizeof(struct sim_htr_gen_cmd), GFP_KERNEL);
	BUG_ON(!hgc);
	(void)env;
	hgc->buf_size = jmdc_sz + ent_md_sz;
	if (hgc->buf_size) {
		hgc->buf = sim_kzalloc(hgc->buf_size, GFP_KERNEL);
		BUG_ON(!hgc->buf);
	}
	{
		struct nvmeibc_disk_gen_cmd *gen_cmd = &hgc->gen_cmd;
		struct nvmeibc_disk_io_command *iocmd = &hgc->iocmd;
		struct nvmeibc_block_command *cmd = &hgc->cmd;
		struct operation *o = &hgc->o;

		cmd->iocmd = iocmd;
		cmd->gen_cmd = gen_cmd;
		cmd->cmdarr = cmd;
		cmd->ncmds = 1;
		cmd->o = o;
		nvmeibc_atomic_set(&cmd->n_uncompleted_cmds, 1);

		iocmd->disk_cmd.owner = cmd;
		iocmd->comp.cmd = cmd;
		iocmd->comp.cmd->gen_cmd = gen_cmd;

		gen_cmd->disk_cmd.owner = iocmd;
		nvmeib_buffer_init_one(&gen_cmd->param.uj.jmdc_dest.local, hgc->buf, jmdc_sz);
		nvmeib_buffer_init_one(&gen_cmd->param.uj.ent_md_dest.local, hgc->buf + jmdc_sz, ent_md_sz);
	}
	return hgc;
}

/**
 * Free resources for mock htr gen command
 */
void destroy_sim_htr_gen_cmd(struct sim_htr_gen_cmd *hgc) {
	if (hgc->buf)
		sim_kfree(hgc->buf);
	hgc->buf = NULL;
	sim_kfree(hgc);
}

/**
 * Generic serjio unitest context constructor
 */
static int serjio_uni_ctx_ctor(struct serjio_uni_ctx *ctx, struct NVMeshSystem *sys) {
	*ctx = (struct serjio_uni_ctx){.env.sys    = sys,
	                               .env.client = sys->clients,
	                               .env.dev    = sys->clients->devs[SERJIO_UNI_VSI.volume],
	                               .env.sraid  = NVMeshSystem_TstPRaid_init_rel(sys, SERJIO_UNI_VSI)};

	ctx->slice_size = ctx->env.sraid.cpr->slice_size;
	ctx->mem_size   = ctx->slice_size * NVMEIBC_SECTOR_SIZE;
	ctx->mem        = sim_kmalloc(ctx->mem_size, GFP_KERNEL);
	BUG_ON(!ctx->mem);
	ctx->magic_pattern = __unitest_fill_blocks_unique_pattern(ctx->mem, ctx->slice_size);
	return 0;
}

/**
 * Generic serjio unitest context destructor
 */
static void serjio_uni_ctx_dtor(struct serjio_uni_ctx *ctx) { sim_kfree(ctx->mem); }

/**
 * Initialize new generic serjio unitest context object and return
 */
struct serjio_uni_ctx *init_serjio_uni_ctx(struct NVMeshSystem *sys) {
	struct serjio_uni_ctx *ctx = sim_kzalloc(sizeof(struct serjio_uni_ctx), GFP_KERNEL);
	BUG_ON(!ctx); /*Memory allocation errors handling is for losers*/
	BUG_ON(serjio_uni_ctx_ctor(ctx, sys));
	return ctx;
}

/**
 * Destroy generic serjio unitest context allocated with init_serjio_uni_ctx
 */
void destroy_serjio_uni_ctx(struct serjio_uni_ctx *ctx) {
	serjio_uni_ctx_dtor(ctx);
	sim_kfree(ctx);
}

/**
 * Initialize serjio read_jmdc test context
 */
static struct serjio_uni_read_jmdc_ctx *init_serjio_uni_read_jmdc_ctx(struct NVMeshSystem *sys) {
	struct serjio_uni_read_jmdc_ctx *ctx = sim_kzalloc(sizeof(struct serjio_uni_read_jmdc_ctx), GFP_KERNEL);
	BUG_ON(serjio_uni_ctx_ctor(&ctx->super, sys));
	ctx->hgc = init_sim_htr_gen_cmd(ctx->super.env,
									NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE * (sizeof(union jblock_md)),
									NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE * sizeof(struct nvmeib_jrnl_ent_md));

	return ctx;
}

/**
 * Destroy serjio read_jmdc test context
 */
void destroy_serjio_uni_read_jmdc_ctx(struct serjio_uni_read_jmdc_ctx *ctx) {
	destroy_sim_htr_gen_cmd(ctx->hgc);
	serjio_uni_ctx_dtor(&ctx->super);
	sim_kfree(ctx);
}

/**
 * Initialize serjio gen_wrap test context
 */
static struct serjio_uni_gen_wrap_ctx *init_serjio_uni_gen_wrap_ctx(struct NVMeshSystem *sys) {
	struct serjio_uni_gen_wrap_ctx *ctx = sim_kzalloc(sizeof(struct serjio_uni_gen_wrap_ctx), GFP_KERNEL);
	BUG_ON(serjio_uni_ctx_ctor(&ctx->super, sys));

	return ctx;
}

/**
 * Destroy serjio gen_wrap test context
 */
void destroy_serjio_uni_gen_wrap_ctx(struct serjio_uni_gen_wrap_ctx *ctx) {
	serjio_uni_ctx_dtor(&ctx->super);
	sim_kfree(ctx);
}

/****************************************************************************/
/*                            TEST SCENARIOS                                */
/****************************************************************************/
/**
 * Alloc jentries according to alloc_bmp
 */
static void __read_jmdc_iter_alloc_entries(struct serjio_uni_read_jmdc_ctx *ctx) {
	struct test_context *env = &ctx->super.env;
	struct serverSimulator *server = &env->sys->servers[ctx->si];
	u32 i;

	ctx->hgc->cuuid = nvmeibc_get_uuid(&env->client->p->core);
	ctx->jri =        nvmeibs_get_jri_by_uuid(server, ctx->hgc->cuuid);

	/*Allocation of dlba start+i yields a unique jentry id, assuming hash_j2d was injected*/
	for (i = 0; i < ARRAY_SIZE(ctx->jentries)/nvmeibc_jentry_num_blocks; ++i) {
		if (ctx->alloc_bmp & (1UL << i)) {
			ctx->jentries[i] = nvmeibc_jam_simu_alloc_entry(server->disk, env->sraid.cpr->dlba_start + i, ec_tx_gen_next_txid(), false);
			BUG_ON(ctx->jentries[i] == (u32)-1);
			/*Compute injection ptrs for newly allocated jentry*/
			ctx->bptrs[i] = serverSimulator_get_block_inject_ptrs(server, env->sraid.cpr->dlba_start, ctx->jri, ctx->jentries[i], 0);
		}
	}
}

/**
 * Abandon jentries according to abandoned_bmp
 */
static void __read_jmdc_iter_abandon_entries(struct serjio_uni_read_jmdc_ctx *ctx) {
	u32 i;
	for (i = 0; i < ARRAY_SIZE(ctx->jentries)/nvmeibc_jentry_num_blocks; ++i) {
		if (ctx->abandoned_bmp & (1UL << i)) {
			nvmeibs_nordda_jour_entry_do(&ctx->super.env.sys->servers[ctx->si], ctx->bptrs[i].jri, ctx->bptrs[i].jentry,
			                             "Abandon entry");
		}
	}
}

/**
 * No abandoned entries at all
 */
static void __read_jmdc_iter_build_testcase_none(struct serjio_uni_read_jmdc_ctx *ctx) {
	unitest_trace_checkpoint(unitest_serjio_check_read_jmdc_api, no_entries);

	/*Allocate and abandon entries 0 and 1*/
	ctx->abandoned_bmp = ctx->alloc_bmp = 0x0;
	__read_jmdc_iter_alloc_entries(ctx);

	/*Abandon entries*/
	__read_jmdc_iter_abandon_entries(ctx);
}

/**
 * All entries are abandoned
 */
static void __read_jmdc_iter_build_testcase_all(struct serjio_uni_read_jmdc_ctx *ctx) {
	u32 i, n_entries = ARRAY_SIZE(ctx->jentries)/nvmeibc_jentry_num_blocks;
	const struct disk_range *seg = &ctx->super.env.sraid.cpr[ctx->si];
	unitest_trace_checkpoint(unitest_serjio_check_read_jmdc_api, all_entries);

	ctx->abandoned_bmp = ctx->alloc_bmp = (1UL << n_entries) - 1;
	__read_jmdc_iter_alloc_entries(ctx);
	for (i = 0; i < n_entries; ++i) {
		nvmeibc_block_dp_ec_jmd_encode(ctx->bptrs[i].jmd, seg->dlba_start, 0x6, 0x6, false, false);
		*ctx->bptrs[i].ram.jmdc = *ctx->bptrs[i].jmd;
	}

	/*Abandon entries*/
	__read_jmdc_iter_abandon_entries(ctx);
}

/**
 * Inject test data into two separate jentries
 */
static void __read_jmdc_iter_build_testcase_two(struct serjio_uni_read_jmdc_ctx *ctx) {
	const u32 si = ctx->si;
	const struct disk_range *seg = &ctx->super.env.sraid.cpr[si];

	unitest_trace_checkpoint(unitest_serjio_check_read_jmdc_api, two_entries);

	/*Allocate and abandon entries 0 and 1*/
	ctx->abandoned_bmp = ctx->alloc_bmp = 0x3;
	__read_jmdc_iter_alloc_entries(ctx);

	/*Inject J2D in raid*/
	jblock_md_jmd_encode_v0(ctx->bptrs[0].jmd, seg->dlba_start, 0x5, 0x5);
	*ctx->bptrs[0].ram.jmdc = *ctx->bptrs[0].jmd;

	/*Inject J2D outbound*/
	jblock_md_jmd_encode_v0(ctx->bptrs[1].jmd, (seg->dlba_start + seg->length), 0x5, 0x5);
	*ctx->bptrs[1].ram.jmdc = *ctx->bptrs[1].jmd;

	/*Abandon entries*/
	__read_jmdc_iter_abandon_entries(ctx);
}

/**
 * This is called after server side work on read_jmdc command is done
 */
static int __read_jmdc_callback(struct nvmeibc_disk_hook_args *args, struct nvmeibc_disk_gen_cmd *gen_cmd) {
	if (gen_cmd->opcode == NVMEIB_GEN_OP_GET_UUID_JOUR) {
		complete(&args->wait_event);
	}
	return 1; /*Break execution flow*/
}

/**
 * Prepare and call server side
 */
static void __read_jmdc_iter_call_cmds(struct serjio_uni_read_jmdc_ctx *ctx) {
	u32 si = ctx->si;
	struct nvmeibc_disk_gen_cmd *gen_cmd = &ctx->hgc->gen_cmd;
	struct nvmeibc_topology *topo;
	int rv;
	struct nvmeibc_disk_hooks disk_hooks = {.before_gen_cmd_comp_cb = __read_jmdc_callback};

	gen_cmd->jiffies_start        = jiffies;
	gen_cmd->timeout              = HZ;
	gen_cmd->opcode               = NVMEIB_GEN_OP_GET_UUID_JOUR;
	gen_cmd->cpu_mask_info        = (struct nvmeib_cpu_mask_info){0};
	gen_cmd->param.uj.client_uuid = *ctx->hgc->cuuid;
	gen_cmd->param.uj.binje       = NVMEIB_EC_INVALID_JOURNAL_BINJE; /* Match all Ns for this UUID */
	BUILD_BUG_ON(ARRAY_SIZE(gen_cmd->param.uj.sgmnt_uuid) != ARRAY_SIZE(ctx->super.env.sraid.tpr->s[si].uuid));
	memcpy(gen_cmd->param.uj.sgmnt_uuid, ctx->super.env.sraid.tpr->s[si].uuid,
	       ARRAY_MEM_SIZE(ctx->super.env.sraid.tpr->s[si].uuid));
	/* Point to correct disk, segment and block device that are responsible for the opration */
	gen_cmd->disk    = ctx->super.env.sys->servers[si].disk;
	topo             = nvmeibc_topology_get(&ctx->super.env.sys->clients->devs[SERJIO_UNI_VSI.volume]->topologies);
	ctx->hgc->cmd.ds = &topo->chunks[SERJIO_UNI_VSI.chunk].raid1s[SERJIO_UNI_VSI.raid].segments[si];
	nvmeibc_topology_put(topo);

	/* Inject callback */
	init_completion(&disk_hooks.args.wait_event);
	nvmeibc_disk_hooks_setup(gen_cmd->disk, &disk_hooks);

	/* Execute the command */
	DEBUG_TRANSFERS_init_cb_counters(1, &ctx->hgc->cmd);
	{
		struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
		rv = icore_ops->execute_gen(icore_ops, &gen_cmd->disk->base, gen_cmd);
	}
	BUG_ON(rv);

	wait_for_completion(&disk_hooks.args.wait_event);

	/*Cleanup*/
	nvmeibc_disk_hooks_clean(gen_cmd->disk);

	return;
}

/**
 * Verify results are as expected
 */
static void __read_jmdc_iter_verify(struct serjio_uni_read_jmdc_ctx *ctx) {
	struct nvmeib_gen_cmd_rs_uj *rsp = &ctx->hgc->gen_cmd.rsp.uj;
	u32 bit;

	/* Basic sanity */
	BUG_ON(ctx->hgc->gen_cmd.comp_code);
	BUG_ON(rsp->jour.rng_nlba != NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE);
	BUG_ON(rsp->jour.rng_id != ctx->jri);

	/* Verify bitmaps match */
	/* 1. For each jentry the we abandoned - rsp is abandoned */
	for (bit = 0; bit < ARRAY_SIZE(ctx->jentries)/nvmeibc_jentry_num_blocks; ++bit) {
		if (ctx->abandoned_bmp & (1UL << bit))                            /*This jentry is abandoned*/
			BUG_ON(!test_bit(ctx->jentries[bit], rsp->jour.abnd_ents_bitmap)); /*Expect abandoned in rsp*/
	}
	/* 2. Number of abandoned entries is the same in both */
	BUG_ON(hweight64(ctx->abandoned_bmp) != hweight64(*rsp->jour.abnd_ents_bitmap));
}

/* Cleanup and verify cleanup was succesfull */
static void __read_jmdc_iter_clean(struct serjio_uni_read_jmdc_ctx *ctx) {
	u32 si = ctx->si, i;
	/*Free entries*/
	for (i = 0; i < ARRAY_SIZE(ctx->jentries)/nvmeibc_jentry_num_blocks; ++i) {
		if (ctx->alloc_bmp & (1UL << i)) {
			nvmeibs_nordda_jour_entry_do(&ctx->super.env.sys->servers[si], ctx->bptrs[i].jri, ctx->bptrs[i].jentry,
			                             "Free");
		}
	}
	NVMeshSystem_serialize(ctx->super.env.sys);
	NVMeshSystem_check_no_abandoned(ctx->super.env.sys);
}

/**
 * Read jmdc test entry point
 */
int unitest_serjio_check_read_jmdc_api(struct NVMeshSystem *sys) {
	extern bool profiling_enabled;
	struct serjio_uni_read_jmdc_ctx *ctx = init_serjio_uni_read_jmdc_ctx(sys);
	bool __profiling_enabled = profiling_enabled;
	profiling_enabled        = false; /*Disable profiling for this test as we do not use full datapath flow*/
	NVMeshSystem_check_no_abandoned(ctx->super.env.sys);
	nvmeibc_inject_jam_hash64(ctx->super.env,
	                          __hash_j2d); /*Inject hash, required for deterministic allocation of jentries*/

	nvmeibc_ensure_jam_has_nothing_bound(ctx->super.env);
	/* Iterating on all segments. Normally, all segments shall behave identically.
	   Alternatively could check only one segment or a random set of segments.*/
	for (ctx->si = 0; ctx->si < ctx->super.env.sraid.cpr->replicas; ++ctx->si) {
		unitest_trace_checkpoint(unitest_serjio_check_read_jmdc_api, next_sgmnt, "@SI", ctx->si);

		__read_jmdc_iter_build_testcase_none(ctx);
		__read_jmdc_iter_call_cmds(ctx);
		__read_jmdc_iter_verify(ctx);
		__read_jmdc_iter_clean(ctx);

		__read_jmdc_iter_build_testcase_two(ctx);
		__read_jmdc_iter_call_cmds(ctx);
		__read_jmdc_iter_verify(ctx);
		__read_jmdc_iter_clean(ctx);

		__read_jmdc_iter_build_testcase_all(ctx);
		__read_jmdc_iter_call_cmds(ctx);
		__read_jmdc_iter_verify(ctx);
		__read_jmdc_iter_clean(ctx);
	}

	nvmeibc_inject_jam_hash64(ctx->super.env,
	                          NULL); /*Restore hash, last __read_jmdc_iter_clean should have cleaned all*/
	destroy_serjio_uni_read_jmdc_ctx(ctx);

	profiling_enabled = __profiling_enabled; /*Restore profiling*/

	return 0;
}

/**
 * Prepare test for specific segment
 */
static void __gen_wrap_prepare_test(struct serjio_uni_gen_wrap_ctx *ctx, u32 si, u32 slba) {
	struct test_context *env = &ctx->super.env;
	ctx->si   = si;
	ctx->dlba = env->sraid.cpr->dlba_start + slba;
	ctx->jri  = nvmeibs_get_jri_by_uuid(&env->sys->servers[ctx->si], nvmeibc_get_uuid(&env->client->p->core));
	if (NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE/nvmeibc_jentry_num_blocks < slba) { // slice must not exceed journal entry number (TODO: fix this logic for binje > 1)
		slba = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE/nvmeibc_jentry_num_blocks - 1;
	}
	/*For this test, it is important to be sure what jentry will be returned by allocation.
	  Problem is, that the test will free this entry multiple times.
	  On each free, serjio will make sure to trigger jam's unbind, hence there is no way to
	  ensure same one will be returned by the next allocation.
	  **EXCEPT** one condition: there are simply no other free entries.
	  So here is the trick: we drain all the free entries at the beginning of the test, leaving exactly 1.*/

	ctx->io2sgmnt2entry = nvmeibc_drain_free_jrnls(ctx->super.env, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, 1);

	/*Choose the one*/
	ctx->jentry = ctx->io2sgmnt2entry[N_MAX_RAID_SLICE_LEN * slba + ctx->si];
	BUG_ON(ctx->jentry == (u32)ILLEGAL_JRNL_ENTRY);
	/*Take full ownership over it*/
	ctx->io2sgmnt2entry[N_MAX_RAID_SLICE_LEN * slba + ctx->si] = ILLEGAL_JRNL_ENTRY;
	nvmeibs_nordda_jour_entry_do(&env->sys->servers[ctx->si], ctx->jri, ctx->jentry, "Abandon entry");
	nvmeibs_nordda_jour_entry_do(&env->sys->servers[ctx->si], ctx->jri, ctx->jentry, "Free");
	NVMeshSystem_serialize(env->sys);
}

/**
 * Advance serjio entry gen id until it equals @dest
 */
static void __gen_wrap_advance_ent_id(struct serjio_uni_gen_wrap_ctx *ctx, u8 dest) {
	struct nvmeibs_serjio_full_gen_id_info full_gen_id_info;
	do {
		BUG_ON(ctx->jentry != (u32)nvmeibc_jam_simu_alloc_entry(ctx->super.env.sys->servers[ctx->si].disk, ctx->dlba, ec_tx_gen_next_txid(), false /*not dry*/));
		nvmeibs_nordda_jour_entry_do(&ctx->super.env.sys->servers[ctx->si], ctx->jri, ctx->jentry, "Abandon entry");
		nvmeibs_nordda_jour_entry_do(&ctx->super.env.sys->servers[ctx->si], ctx->jri, ctx->jentry, "Free");
		NVMeshSystem_serialize(ctx->super.env.sys);
		nvmeibs_serjio_get_full_gen_id_info(&ctx->super.env.sys->servers[ctx->si].ramDisk.server_disk.di, ctx->jri, ctx->jentry, &full_gen_id_info);
	} while (full_gen_id_info.ent_gen_id != dest);
}

/**
 * Test actual wrap
 * @note Assumes focus jentry gen_id is exactly 1 before wrap
 */
static void __gen_wrap_wrap(struct serjio_uni_gen_wrap_ctx *ctx) {
	struct nvmeibs_serjio_full_gen_id_info full_gen_id_info_before, full_gen_id_info_after;

	/*Take a snapshot of gen ids*/
	nvmeibs_serjio_get_full_gen_id_info(&ctx->super.env.sys->servers[ctx->si].ramDisk.server_disk.di, ctx->jri,
	                                    ctx->jentry, &full_gen_id_info_before);

	BUG_ON(ctx->jentry != (u32)nvmeibc_jam_simu_alloc_entry(ctx->super.env.sys->servers[ctx->si].disk, ctx->dlba,
	                                                        ec_tx_gen_next_txid(), false /*not dry*/));
	nvmeibs_nordda_jour_entry_do(&ctx->super.env.sys->servers[ctx->si], ctx->jri, ctx->jentry, "Abandon entry");
	nvmeibs_nordda_jour_entry_do(&ctx->super.env.sys->servers[ctx->si], ctx->jri, ctx->jentry, "Free");
	NVMeshSystem_serialize(ctx->super.env.sys);

	/*Take a snapshot of gen ids*/
	nvmeibs_serjio_get_full_gen_id_info(&ctx->super.env.sys->servers[ctx->si].ramDisk.server_disk.di, ctx->jri,
	                                    ctx->jentry, &full_gen_id_info_after);

	/*See how it went*/
	BUG_ON(full_gen_id_info_after.ent_gen_id != nvmeib_jrnl_ent_gen_id_min);
	BUG_ON(full_gen_id_info_before.jri_gen_id + 1 != full_gen_id_info_after.jri_gen_id);
	BUG_ON(full_gen_id_info_after.wrapped); /*Serjio should handle wrap by this point*/
}

/**
 * Teststability of the system after wraparound using some simple IO
 */
static void __gen_wrap_test_stability(struct serjio_uni_gen_wrap_ctx *ctx) {
	/*Start write, it will stall since the entries are allocated*/
	REPORT_ERROR(osSimulator_writeArr(&ctx->super.env.client->OS, SERJIO_UNI_VSI.volume, 0, ctx->super.slice_size,
	                                  ctx->super.mem));
	/*Release all the entries we are holding to allow IO to complete*/
	nvmeibc_release_drained_jrnls(ctx->super.env, ctx->io2sgmnt2entry, NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, NVMEIBC_JIDX_EVT_END_USE);
	/*Verify all writes have completed*/
	clientSimulator_wait_for_all_bio_ops(ctx->super.env.client);
	/*Verify that the data was written by reading it*/
	memset(ctx->super.mem, 0, ctx->super.mem_size);
	REPORT_ERROR(osSimulator_readArrWait(&ctx->super.env.client->OS, SERJIO_UNI_VSI.volume, 0, ctx->super.slice_size,
	                                     ctx->super.mem));
	__unitest_verify_blocks_pattern(ctx->super.mem, ctx->super.slice_size, ctx->super.magic_pattern,
	                                false); /*Verify that read and write matched*/
	/*See we are OK*/
	NVMeshSystem_serialize(ctx->super.env.sys);
	NVMeshSystem_check_no_abandoned(ctx->super.env.sys);
}

/**
 * Test cleanup
 */
static void __gen_wrap_cleanup(struct serjio_uni_gen_wrap_ctx *ctx) {
	(void)ctx; /*Nothing :( , maybe in the future though*/
}

/**
 * Entry gen id wraparound entry point
 */
static int unitest_serjio_gen_wrap(struct NVMeshSystem *sys) {
	struct serjio_uni_gen_wrap_ctx *ctx = init_serjio_uni_gen_wrap_ctx(sys);

	/*This test will not work as async*/
	nvmeibc_backup_switch_sync_mode(true);

	__gen_wrap_prepare_test(ctx, SERJIO_UNI_VSI.segment, 5); /*Run on slba 5. Why? Because.*/

	unitest_trace_checkpoint(unitest_serjio_gen_wrap, reach_wraparound);
	__gen_wrap_advance_ent_id(ctx, 255); /*Anvance until just before wraparound.*/

	unitest_trace_checkpoint(unitest_serjio_gen_wrap, wrap);
	__gen_wrap_wrap(ctx);

	unitest_trace_checkpoint(unitest_serjio_gen_wrap, test_stability);
	__gen_wrap_test_stability(ctx);

	/*Cleanup*/
	__gen_wrap_cleanup(ctx);
	nvmeibc_restore_sync_mode();
	destroy_serjio_uni_gen_wrap_ctx(ctx);

	return 0;
}

int unitest_serjio_AllTests(struct NVMeshSystem *sys) {
	int rv        = 0;
	u32 rand_seed = jiffies;

	NVMeshSystem_all_clients_dbg_di(sys, true);

	srand(rand_seed);
	unitest_trace_checkpoint(unitest_serjio_check_read_jmdc_api, rand_seed, "@RAND_SEED", rand_seed);

	rv |= SIMU_RUN_TEST(unitest_serjio_check_read_jmdc_api, sys);
	rv |= SIMU_RUN_TEST(unitest_serjio_gen_wrap, sys);

	return rv;
}
