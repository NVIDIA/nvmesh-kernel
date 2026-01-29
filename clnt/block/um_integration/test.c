/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmesh_dp_lib_api.h"
#include "nvmeibc_disk_locks.h"
/************************ Transport layer dependency **********************************/
static void _vf_run_cmpxchg(struct nvmeibc_disk* disk, u64 address, struct nvmeibc_d_rdma_comp *dc) {
	union nvmeib_blkset_info bi = {.bits.txid = 13, .bits.dirty = 0 };
	const u64 current_lock_val = dc->compare;	// Compare exchange will always succeed
	dc->lock.id = current_lock_val;
	dc->lock.bi = bi.all;
	dc->lock_status = (dc->lock.id == dc->compare) ? NCL_STATUS_TAKEN : NCL_STATUS_CONTENDED;
	dc->callback(dc);
}

static void _vf_run_viewlock(struct nvmeibc_disk* disk, u64 address, struct nvmeibc_d_rdma_comp *dc) {
	union nvmeib_blkset_info bi = {.bits.txid = 13, .bits.dirty = 0 }; //not read
	const u64 current_lock_val = 0;	// View lock will always see it as unlocked
	dc->lock.id = dc->compare;
	dc->lock.bi = bi.all;
	dc->lock_status = (dc->lock.id == current_lock_val) ? NCL_STATUS_TAKEN : NCL_STATUS_CONTENDED;
	dc->callback(dc);
}

static void _vf_run_write_binfo(struct nvmeibc_disk* disk, u64 address, struct nvmeibc_d_rdma_comp *dc) {
	(void)disk; (void)address;
	dc->lock_status = NCL_STATUS_TAKEN;				// Write binfo always succeeds
	dc->callback(dc);
}

static void _vf_run_io_blocks(struct nvmeibc_disk* disk, struct nvmeibc_disk_io_command* dcmd) {
	(void)disk;
	dcmd->comp.comp_code = 0;	// IO always succeeds
	nvmeibc_block_completion(&dcmd->comp);
}

static int _vf_get_slr_status(struct stale_lock_resolver_t *slr, u32 lock_id) {
	(void)slr; (void)lock_id;
	return 0;					// Always safe to take over stale lock
}

static int _vf_get_slr_cuuid_by_lockid(struct stale_lock_resolver_t *slr, u32 lock_id, uuid_be *cuuid) {
	(void)slr; (void)lock_id;
	memset(cuuid, 0xFF, sizeof(*cuuid));
	return 0;					// Always get dummy uuid
}

#include <stdarg.h>
int _vfprintk(const char *fmt, va_list args) {
	return vfprintf(stderr, fmt, args);
}

/************************************ Recovery testing **********************************/
#include "toma/clnt/nvmeibt_client_protocol.h"
#include "block/recovery/nvmeibc_raid_recovery.h"
#define pr_recov(fmt, ...) ({ if (0) pr_info("Recov: " fmt, ##__VA_ARGS__); })
void _vf_get_problems(struct nvmeibc_disk *disk, u64 dlba_start, u64 blocksets_length, struct nvmeibc_d_rdma_comp *dc) {
	int i, _bs = max((int)DIV_ROUND_UP(blocksets_length, 3ULL), 3);		// Reply 1/3 of requested range, at least 3 blocksets
	int reply_batch_size = min(_bs, (int)blocksets_length);
	union nvmeib_blkset_problem_report *arr = kzalloc(sizeof(*arr)*reply_batch_size, 0);
	pr_recov("get_problems=[%u..%u)\n", (int)dlba_start, (int)(dlba_start + reply_batch_size));
	dc->dbits_arr.size = (u64)reply_batch_size;	// All dirty
	dc->dbits_arr.arr = (u8*)arr;
	for (i = 0; i < reply_batch_size; i++) {
		arr[i].dbits = nvmeib_dbits_entry_build_unk(-1, -1).all_bits;
		arr[i].binfo_not_commited = (u16)1;
	}
	dc->lock_status = NCL_STATUS_TAKEN;
	dc->callback(dc);
	kfree(arr);			// DPlib code should have copied this already
	memset(arr, 0xFF, sizeof(*arr)*reply_batch_size);
	dc->dbits_arr.size = -1UL;
}

void  _vf_read_jmdc(struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp *dc) {
	pr_recov("read jmdc %s\n", disk->name);
	// Todo: Fill in here dummy jmdc to trigger cold recovery crac
	dc->rsp.status = NCL_STATUS_TAKEN;
	dc->callback(dc);
}

void _vf_reply_for_toma(enum NVMEIBT_CLIENT_MSG_TYPES msg_type, struct nvmeibt_client_recovery_status_pl *pl) {
	const char c = (msg_type == NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH) ? 'F' : ((msg_type == NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS) ? 'P' : '?');
	pr_recov("%c, 0x%x[%u], b=%u, %u%%,\t\t rv=%d, prv=0x%x, remain=0x%llx\n", c, (u32)pl->task.id, pl->task.type, (u32)pl->task.max_batch_size, (u32)pl->task.effort_percents, pl->ret_code, pl->praid_version, pl->num_locks_left);
}

struct __test_rhooks {
	struct lib_call_api_recov *lib_api;
	int n_syncs, n_batches, in_air_recovs;				// Counters
	int force_abort_after_n_syncs;
	int force_ping_on_n_sync;
} _th;
static void _vf_on_fin_1_sync(struct lib_call_api_recov* ptr) {
	struct __test_rhooks *th = &_th;
	BUG_ON(ptr != th->lib_api);
	th->n_syncs++;
	pr_recov("\t\tsync[%u].done\n", th->n_syncs);
	if (th->n_syncs >= th->force_abort_after_n_syncs) {
		pr_recov("\t\t\t\\-->force abort!\n");
		nvmesh_dp_lib_do_recovery_abort(th->lib_api);
	} else if ((th->force_ping_on_n_sync != 0) && ((th->n_syncs % th->force_ping_on_n_sync) == 0)) {
		th->lib_api->recov_params.effort_percents -= 10;		// Update effort
		pr_recov("\t\t\t\\-->force ping\n");
		nvmesh_dp_lib_do_recovery_ping(th->lib_api);
	}
}

void __test_rhooks_init(struct lib_call_api_recov* ctx) {
	struct __test_rhooks *th = &_th;
	memset(th, 0, sizeof(*th));
	th->force_abort_after_n_syncs = INT_MAX;
	th->lib_api = ctx;
}

/************************************ Main for testing **********************************/
#define N_HTRS (10)
#define MY_BINJE (16)
static int MY_N_PARITIES = 0, MY_SLICE_SIZE = 0, MY_N_SEG = 0, deg_seg_ind = -1;
enum NVMEIBTC_DS_MODE deg_seg_state = NVMEIBTC_DS_MODE_INVALID;
struct nvmeibc_datapath_syncs_resources dpsr;
struct nvmeibc_flow_counters fctr;
struct lib_call_api_sync  my_so;
struct lib_call_api_io    my_io;
struct lib_call_api_recov my_rr;
struct nvmeibc_disk _disks[N_MAX_RAID_SLICE_LEN];

static void __init_transport(struct lib_call_api_params_generic *gen) {
	int i, li;
	MY_N_SEG = MY_SLICE_SIZE + MY_N_PARITIES;
	for (i = 0; i < MY_N_SEG; i++ ) {
		struct seg_params_t *seg = &gen->pr.segs[i];
		struct nvmeibc_disk *disk = &_disks[i];
		struct nvmeibc_disk_client_journal *j = &disk->jour;
		struct lock_ownership_map *l = &seg->lmap;
		strlcpy(disk->name, "Disk0", sizeof(disk->name));
		disk->name[4] += i;
		strlcpy(disk->full_name, disk->name, sizeof(disk->name));
		disk->sector_shift = 12;
		disk->max_request_size_bytes = (1 << 17);
		disk->access_local = false;
		j->rng_id = i;
		j->rng_slba = 0;
		j->rng_nlba = 512;
		j->rng_gen_id = 100 + i;
		j->n_ents = 8;
		j->rng_nblk = 8;
		j->max_rng_blk = 8;
		j->tot_n_rng = 4;
		strlcpy(j->serjio_boot_id, "Serjio", sizeof(j->serjio_boot_id));
		j->rng_binje = MY_BINJE;
		seg->disk = disk;
		seg->dlba_start =  (i << 10);		// Segment offset i*4[Mb]  = i*1[K]blocks
		seg->dlba_length = (1 << 20);		// Segment length is 4[GB] = 1[M]blocks
		seg->sync_safety = NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO;
		seg->toma_acm = (i == deg_seg_ind) ? deg_seg_state : NVMEIBTC_DS_MODE_RW;
		if (deg_seg_state != NVMEIBTC_DS_MODE_DEAD)
			l->n_locks = MY_N_PARITIES + 1;
		else
			l->n_locks = MY_N_PARITIES;
		for (li = 0; li < l->n_locks; li++) {
			l->type[li] = ((li == 0) ? NVMEIBTC_DS_OWNER_MODE_PRIMARY : NVMEIBTC_DS_OWNER_MODE_COPY_OWNER);
			if (deg_seg_ind == i) {
				l->si[li] = (i - 1 - li + MY_N_SEG) % MY_N_SEG;
			} else {
				l->si[li] = (i -     li + MY_N_SEG) % MY_N_SEG;
			}
		}
	}
	memset(&dpsr, 0, sizeof(dpsr));
	memset(&fctr, 0, sizeof(fctr));
	atomic_set(&fctr.htrs.n_calls, N_HTRS);	// To verify that it is not zeroed but appended
}

static void __init_my_so_or_rw(enum nvmeib_block_io_op op, struct lib_call_api_params_generic* pg) {
	__init_transport(pg);
	pg->bdev = (struct bdev_params_t){ .binje = MY_BINJE, .enable_crc_check = 1, .use_debug_di = 0};
	pg->pr.topology_id = 0x56;
	pg->pr.praid_version = 0x57;
	pg->pr.conf_version = 0x23;
	pg->pr.n_segments = MY_N_SEG;
	pg->pr.slice_size = MY_SLICE_SIZE;
	pg->pr.lid = (union nvmeib_lock_id){.all = 0x1717 };
	// pg->pr.slr = NULL;	// Already alloced
	pg->pr.offending_stale_lock.all = 0;
	if (op == NVMEIB_BLOCK_IO_OP_RECOVER_STALE)
		pg->pr.offending_stale_lock = ((union nvmeib_lock_id){.bits.lock_id = 13, .bits.is_stale = 1 });
	pg->dbg_id = 1000 + op;
	pg->op = op;
	my_rr.stats.sync = my_io.stats.sync = my_so.stats.sync = &dpsr.stats;
	my_rr.stats.fctr = my_io.stats.fctr = my_so.stats.fctr = &fctr;
}

static void __init_my_so(enum nvmeib_block_io_op op) {
	struct lib_call_api_params_generic* pg = &my_so.gen_params;
	struct lib_call_api_params_sync* ps = &my_so.sync_params;
	__init_my_so_or_rw(op, pg);
	ps->rlba = pg->pr.slice_size * 0x100;
	ps->start_slice = 0;
	ps->n_slices = (op == NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE) ? 0 : 32;
}

static void __init_my_rw(enum nvmeib_block_io_op op, struct bio *bio) {
	struct lib_call_api_params_generic* pg = &my_io.gen_params;
	struct lib_call_api_params_io*      pi = &my_io.io_params;
	__init_my_so_or_rw(op, pg);
	pi->bio = bio;
	pi->enable_local_read_optimization = false;
}

static void __init_my_recov(enum NVMEIBT_RECOVERY_TYPE op, int start_lock, int num_locks) {
	struct lib_call_api_params_generic* pg = &my_rr.gen_params;
	struct lib_call_api_params_recov* rrp = &my_rr.recov_params;
	static int recov_id_gen = 0xAA;
	__init_my_so_or_rw(NVMEIB_BLOCK_IO_OP_NOP, pg);
	rrp->id = ++recov_id_gen;
	rrp->type = (u8)op;
	rrp->effort_percents = 100 - 30*(recov_id_gen&0x1);
	rrp->is_mandatory = (op != NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC);
	rrp->do_only_owners = (op != NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC);;
	rrp->start_lock = start_lock;
	rrp->num_locks =  num_locks;
	rrp->surviving_ram_bmp_cold_recov = -1U;
	rrp->seg_id = (u8)(recov_id_gen % MY_N_SEG);
	__test_rhooks_init(&my_rr);
	pr_recov("-------------------launch %s, seg[%d]\n", nvmeibt_recov_type_to_3str(op), rrp->seg_id);
}

static void __my_rw_set_locks_taken(bool taken) {
	struct lib_call_api_params_generic* pg = &my_io.gen_params;
	u32 i;
	for (i = 0; i < pg->pr.n_segments; i++ ) {
		 pg->pr.locks[i].is_already_taken = taken;
		 pg->pr.locks[i].binfo = (union nvmeib_blkset_info){.bits.txid = 0x1187, .bits.dirty = 0 };
	}
}

static void __print_hooray(void) {
	#ifdef NDEBUG
		const char *opt = "Release";
	#else
		const char *opt = "Debug";
	#endif
	pr_emerg("unitest of nvmesh_dp_lib: %s, git=%s(%s:0x%llx), Version=%s, Optimization:%s\n", __stringify(NVMEIB_COMPILATION_DATE), __stringify(VER_TAGID), __stringify(BRANCH_NAME), COMMIT_ID, __stringify(NVMESH_RELEASE), opt);
}

extern int64_t get_num_memory_allocs(void);
static void run_syncs_unitest(void) {
	const int64_t num_allocs = get_num_memory_allocs();
	const enum nvmeib_block_io_op* op = &my_so.gen_params.op;
	pr_info("Syncs test started\n");
	if (1) {	// Jbod:
		MY_N_PARITIES = 0; MY_SLICE_SIZE = 1;
		__init_my_so(NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error != 0);
	}

	MY_N_PARITIES = 1; MY_SLICE_SIZE = 1;
	if (1) {	// R1: RW,RW, Scrubbing
		__init_my_so(NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING);
		BUG_ON(num_allocs != get_num_memory_allocs());		// Does not require memory allocs
		BUG_ON(atomic_read(&my_so.stats.fctr->nowh.n_other_fix) != 0);
		BUG_ON(atomic_read(&my_so.stats.fctr->htrs.n_calls) != N_HTRS);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error != 0);
		BUG_ON(my_so.out.binfo.bits.txid != (int)*op);
		BUG_ON(num_allocs != get_num_memory_allocs());
		BUG_ON(atomic_read(&my_so.stats.fctr->nowh.n_other_fix) != 1);
		BUG_ON(atomic_read(&my_so.stats.fctr->htrs.n_calls) != N_HTRS);
	}
	if (1) {	// R1: Illegal syncs
		__init_my_so(NVMEIB_BLOCK_IO_OP_REC_R1_CONV_STALE2DB);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error == 0);
		__init_my_so(NVMEIB_BLOCK_IO_OP_RECOV_PROBLEM);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error == 0);
	}
	if (1) {	// R1: RW,RW Stale lock
		__init_my_so(NVMEIB_BLOCK_IO_OP_RECOVER_STALE);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error != 0);
		BUG_ON(my_so.out.binfo.bits.txid != (int)*op);
	}
	if (1) {
		// R1: RW,RW, Commit stale
		__init_my_so(NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error != 0);
	}

	deg_seg_ind = 0;
	if (1) {	// R1: D,RW Stale lock -> Stale2Dirty
		deg_seg_state = NVMEIBTC_DS_MODE_DEAD;
		__init_my_so(NVMEIB_BLOCK_IO_OP_RECOVER_STALE);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error != 0);
	}
	if (1) {	// R1: D,RW, Dirty-convict turn on
		deg_seg_state = NVMEIBTC_DS_MODE_W_IS_DIRTY;
		__init_my_so(NVMEIB_BLOCK_IO_OP_REC_DCONVICT_TURN_ON);
		nvmesh_dp_lib_do_sync_op(&my_so);
		BUG_ON(my_so.out.error != 0);
		BUG_ON(my_so.out.binfo.bits.txid != (int)*op);
		BUG_ON(my_so.out.binfo.bits.dirty == 0);
	}
	BUG_ON(num_allocs != get_num_memory_allocs());
	pr_info("Syncs test end\n");
}

/************************************ RW IO **********************************/
static struct bio* __create_bio(u64 start, u64 n_blks, u8 arr[], enum bio_req_io_types action) {
	const size_t neededMem = sizeof(struct bio) + n_blks * (sizeof(struct bio_vec)+sizeof(struct page));	// Amount of memory needed for bio
	struct bio     *res   = (struct bio*)kzalloc(neededMem, 0);		// Todo: Use bio_alloc()
	struct bio_vec *vecs  = (struct bio_vec *)(res+1);				// Memory after struct bio is given to array vecs
	struct page    *pages = (struct page    *)(vecs+n_blks);		// Array of pages starts after the array of vecs
	int i;
	unsigned int n_bvecs = 0;
	BUG_ON(PAGE_SIZE < NVMEIBC_SECTOR_SIZE);
	res->bi_rw = action;
	res->bi_idx = 0;
	res->bi_io_vec = vecs;
	res->bi_sector = (start  * (NVMEIBC_SECTOR_SIZE/512));			// Offset in units of kernel sectors
	res->bi_size =   (n_blks *  NVMEIBC_SECTOR_SIZE);				// Length in bytes
	if (!arr) {														// Trim
		res->bi_io_vec = NULL;										// Verify our code does not use io_vec
		return res;
	}
	for (i = 0; i < (int)n_blks; i++) {
		vecs[ i].bv_page =   &(pages[i]);						// The page with reference to part of 'arr' array for the IO
		vecs[ i].bv_len =    NVMEIBC_SECTOR_SIZE;				// Each vec is an IO of a single block of 4K or 8 blocks of 512[b]
		vecs[ i].bv_offset = 0;
		pages[i].mapped_vaddr = &arr[i* NVMEIBC_SECTOR_SIZE];	// Page 'i' covers the [i,i+1,...i+b] part of the array
		n_bvecs++;
	}
	res->bi_vcnt = n_bvecs;
	return res;
}
static void __bio_set_vlba(struct bio* bio, u64 vlba) { bio->bi_sector = vlba*(NVMEIBC_SECTOR_SIZE/512); }

void run_biorw_unitest(void) {
	const int num_4k_blocks = 2;
	const int memSize = num_4k_blocks * NVMEIBC_SECTOR_SIZE;
	u8* write_mem = kmalloc(memSize, GFP_KERNEL);		// Array to read/write to disk
	struct bio *bio = __create_bio(10, num_4k_blocks, write_mem, WRITE);
	int64_t num_allocs = get_num_memory_allocs();
	int caller_holds_locks;

	pr_info("IO test started\n");
	deg_seg_ind = -1;
	MY_N_PARITIES = 1; MY_SLICE_SIZE = 1;
	if (1) {	// R1: RW,RW, Write
		__bio_set_vlba(bio, 10);
		bio->bi_rw = WRITE;
		__init_my_rw(NVMEIB_BLOCK_IO_OP_WRITE, bio);
		for (caller_holds_locks = 1; caller_holds_locks >= 0; --caller_holds_locks) {
			__my_rw_set_locks_taken(caller_holds_locks);
			nvmesh_dp_lib_do_rwt_op(&my_io);
			BUG_ON((my_io.out.error != 0) || (num_allocs != get_num_memory_allocs()));
		}
	}
	if (1) {	// R1: RW,RW, Read
		__bio_set_vlba(bio, 7);
		bio->bi_rw = READ;
		my_io.gen_params.op = NVMEIB_BLOCK_IO_OP_READ;
		for (caller_holds_locks = 1; caller_holds_locks >= 0; --caller_holds_locks) {
			__my_rw_set_locks_taken(caller_holds_locks);
			nvmesh_dp_lib_do_rwt_op(&my_io);
			BUG_ON((my_io.out.error != 0) || (num_allocs != get_num_memory_allocs()));
		}
	}
	if (1) {	// R1: RW,RW, Trim
		__bio_set_vlba(bio, 9);
		bio->bi_rw = REQ_DISCARD;
		my_io.gen_params.op = NVMEIB_BLOCK_IO_OP_DISCARD;
		nvmesh_dp_lib_do_rwt_op(&my_io);
		BUG_ON(my_io.out.error != -EPERM);
		BUG_ON(num_allocs != get_num_memory_allocs());

		nvmesh_dp_lib_do_trim_op(&my_io);
		BUG_ON(my_io.out.error != -EPERM);
		BUG_ON(num_allocs != get_num_memory_allocs());
	}
	kfree(write_mem);
	kfree(bio);
	pr_info("IO test end\n");
}

void run_recovery_unitest(void) {
	struct __test_rhooks *th = &_th;
	int64_t num_allocs = get_num_memory_allocs();
	pr_info("Recov test started\n");
	deg_seg_ind = -1;
	MY_N_PARITIES = 1; MY_SLICE_SIZE = 1;
	if (1) {	// R1:
		MY_N_PARITIES = 1; MY_SLICE_SIZE = 1;
		__init_my_recov(NVMEIBT_RECOVERY_TYPE_SCRUBBING, 0, 11);
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));
		BUG_ON(atomic_read(&my_rr.stats.fctr->nowh.n_other_fix) != 5);	// 11:2

		// Force ping and update speed
		__init_my_recov(NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, 4, 12);
		th->force_ping_on_n_sync = 3;
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));
		BUG_ON(atomic_read(&my_rr.stats.fctr->nowh.n_dbits_fix) != 6);	// 11:2

		__init_my_recov(NVMEIBT_RECOVERY_TYPE_STALE_REBUILD, 0, 2);
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));

		deg_seg_ind = 1;
		__init_my_recov(NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON, 18, 3);
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));
		BUG_ON(my_rr.stats.sync->num_full_blockset_ok   != (u64)my_rr.recov_params.num_locks);
		BUG_ON(atomic_read(&my_rr.stats.fctr->main.n_dconvict_turnon) != (int)my_rr.recov_params.num_locks);
		deg_seg_ind = -1;
	}
	if (1) {	// EC:
		MY_N_PARITIES = 4; MY_SLICE_SIZE = 2;
		__init_my_recov(NVMEIBT_RECOVERY_TYPE_EC_COLD, 0, 24);
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));

		__init_my_recov(NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC, 15, 24);
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));

		__init_my_recov(NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO, 2, 4);
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));
	}
	if (1) {	// Recovery abort
		__init_my_recov(NVMEIBT_RECOVERY_TYPE_SCRUBBING, 0, 200);
		th->force_abort_after_n_syncs = 1;
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != -10038) || (num_allocs != get_num_memory_allocs()));
		BUG_ON(my_rr.out.err_info.was_canceled_by_caller != true);
	}
	if (1) {
		__init_my_recov(NVMEIBT_RECOVERY_TYPE_VOID_DUMMY, 0, 200);
		nvmesh_dp_lib_do_recovery_op(&my_rr);
		BUG_ON((my_rr.out.error != 0) || (num_allocs != get_num_memory_allocs()));
	}
	pr_info("Recov test end\n");
}

int main(void) {
	int64_t num_allocs = get_num_memory_allocs();
	const struct nvmesh_dp_lib_virtual_table vt = {.run_cmpxchg = _vf_run_cmpxchg, .run_viewlock = _vf_run_viewlock,
		.run_write_binfo = _vf_run_write_binfo, .run_io_blocks = _vf_run_io_blocks,
		.get_slr_status = _vf_get_slr_status, .get_slr_cuuid_by_lockid = _vf_get_slr_cuuid_by_lockid,
		.run_printk = _vfprintk,
		.recov = {.get_problems = _vf_get_problems, .read_jmdc = _vf_read_jmdc, .reply_for_toma = _vf_reply_for_toma, .yield_after_1_sync = _vf_on_fin_1_sync},
		};
	BUG_ON(num_allocs != 0);
	nvmesh_dp_lib_create(vt);
	__print_hooray();
	run_syncs_unitest();
	run_biorw_unitest();
	run_recovery_unitest();

	// Finish
	nvmesh_dp_lib_destroy();
	BUG_ON(0 != get_num_memory_allocs());
	nvmesh_dp_lib_create(vt);
	nvmesh_dp_lib_destroy();
	pr_emerg("unitests done!\n");
	return 0;
}
