/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

// For documentation, see Header in H file
/*****************************************************************************/
// Includes
#include "./server/nvmeibs_main_sim.h"
#include "nvmeib_common_all.h"

#include "nvmeibc_volume.h"
#include "nvmeibc_pausable.h"
#include "nvmeibc_jam.h"
#include "./uni_framework/bunitest_conf.h"
#include "nvmeibc_targets.h"

// The includes below are breaking encapsulation concept (Simulator digs into Blocks code). This is done for debug purpose (verify internal states of Block locks).
#include "../nvmeibc_topology.h"
#include "../nvmeibc_block_common.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_common.h"
#include "../datapath_ec/nvmeibc_block_dp_ec_journal_common.h"
#include "../datapath_ec/nvmeibc_block_dp_ec.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "nvmeibc_disk_gen_cmds.h"

uint nvmeibc_skip_lock_cmds_flags = 0;
uint nvmeibc_skip_disk_iocmds_flags = 1;	// Meaningless, put !0 to prevent pausable layer error prints in __put_pause_state()

/* disk work item */
struct disk_workq {
	struct workqe_struct work;
	struct nvmeibc_disk *disk;
	void *work_data;
};

/************************** Dummy nvmeibc_disk.c functions ********************/
const char *nvmeibc_disk_update_type_str(enum nvmeibc_disk_update_type update_type){ (void)update_type; return "Unknown"; }
int nvmeibc_disk_update_config(struct nvmeibc_disk *disk, struct nvmeibc_disk_update_data *update_data, bool in_interrupt){ (void)disk; (void)update_data; (void)in_interrupt; return 0; }

ssize_t nvmeibc_disk_print_info(struct nvmeibc_disk *disk, char *buf, int len){ /* Print info of disk in JSON format */
	#define BUF_ADD(...)	count += scnprintf(buf+count, len-count, __VA_ARGS__)
	ssize_t count = 0;
	BUF_ADD("{\"name\":\"%s\"}\n", disk->name);
	return count;
	#undef BUF_ADD
}

/*********************** RDMA-ops-Error injection mechanism *******************/
static int inject_trerr_failure_errors[] = {-EAGAIN, -1, -EINVAL, -ENODEV, -EPIPE, -EIO, -ENOMEM};	// Different types of errors that transport layer returns. Look in the original code for more info
#define NUMBER_OF_INJECTABLE_ERRORS ((sizeof(inject_trerr_failure_errors)/sizeof(inject_trerr_failure_errors[0])))


// Error will be injected, increment error count and if limit is reached reset trerr so no more injections will occur
static inline void __inj_trerr_check_and_inject_limited_errors(struct transport_err_injection *trerr) {
	trerr->n_errs_inj++;
	BUG_ON(trerr->n_max_errors_to_inject && trerr->n_max_errors_to_inject < trerr->n_errs_inj);	// There can't be more errors injected than 'n_max_errors_to_inject'
	if (trerr->n_max_errors_to_inject == trerr->n_errs_inj) {	// These will cause error injection to stop from now on
		trerr->inject_trerr_failure_type = 0;
		trerr->inject_trerr_should_rand	= false;
		trerr->enable_lock_errors = false;
	}
}
static inline bool __inj_trerr_is_needed(struct transport_err_injection *trerr, enum INJECT_TRANSPORT_ERROR err_type, struct nvmeibc_d_rdma_comp *comp) {

	if (trerr->inject_trerr_should_rand)
		trerr->inject_trerr_failure_type = ((trerr->inject_trerr_failure_type+1)&INJECT_TRANSPORT_ERROR_CYCLE_SIZE);

	if ((!trerr->enable_lock_errors) && (trerr->inject_trerr_failure_type & INJECT_TRANSPORT_ERROR_LOCK_ERROR))
		return false;

	if ((!trerr->enable_io_errors) && (trerr->inject_trerr_failure_type & INJECT_TRANSPORT_ERROR_IO_ERROR))
		return false;

	if (trerr->inject_trerr_failure_type == err_type) {
		if (trerr->n_errs_skipped < trerr->n_max_errors_to_skip) {
			trerr->n_errs_skipped++;
			return false;
		}
		if (comp)	/* Save the injected error in an unused field of transport */
			comp->send_id = trerr->inject_trerr_failure_type;
		__inj_trerr_check_and_inject_limited_errors(trerr);
		return true;
	}
	return false;
}

int inject_transport_error(struct nvmeibc_disk_hook_args *hook_args, struct nvmeibc_d_rdma_comp *dc, bool *should_return, enum INJECT_TRANSPORT_ERROR err_type) {
	struct transport_err_injection *trerr = &hook_args->trerr;
	if (__inj_trerr_is_needed(trerr, err_type, dc)) {
		*should_return = true;
		return inject_trerr_failure_errors[rand()%NUMBER_OF_INJECTABLE_ERRORS];	// Return a random error
	} else {
		*should_return = false;
		return -1;
	}
}

/****************************** nvmeibc_pausable.c API ************************/
#include "server/nvmeibs_nic_dma_atomics.h"

#define getDiskByHandle(H)		((struct nvmeibc_disk_used_lock_segment *)H)->disk
#define getRamDiskByHandle(H)	(&serverOf(getDiskByHandle(H))->ramDisk)

int nvmeibc_disk_locks_write_blkset_info(void *handle, u64 addr, struct nvmeibc_d_rdma_comp *dc){
	on_disk_hook_return(nvmeibc_disk_locks_write_blkset_info, getDiskByHandle(handle), inject_transport_error, dc, INJECT_TRANSPORT_ERROR_BINFO_WR);
	dc->opr = NVMEIBC_LOCK_BLKSET_INFO_WRITE;
	return serverRam_dma_do_db_txid(getRamDiskByHandle(handle), addr, dc);
}

int nvmeibc_disk_locks_read_blkset_info(void *handle, u64 addr, struct nvmeibc_d_rdma_comp *dc){
	on_disk_hook_return(nvmeibc_disk_locks_read_blkset_info, getDiskByHandle(handle), inject_transport_error, dc, INJECT_TRANSPORT_ERROR_BINFO_RD);
	dc->opr = NVMEIBC_LOCK_BLKSET_INFO_READ;
	return serverRam_dma_do_db_txid(getRamDiskByHandle(handle), addr, dc);
}

static void nvmeibc_disk_local_gen_async_cb(union nvmeib_gen_cmd_rsp *rsp, int rv){ (void)rsp; (void)rv; }

int nvmeibc_disk_execute_gen(struct nvmeibc_disk *disk, struct nvmeibc_disk_gen_cmd *gen_cmd) {
	struct serverSimulator *S = serverOf(disk);
	if ((disk->disk_hooks) && ((disk->disk_hooks->args.trerr.enable_jentry_erase_errors) || (gen_cmd->opcode != NVMEIB_GEN_OP_JENTRY_ERASE))) {
		struct nvmeibc_d_rdma_comp * comp = NULL;
		on_disk_hook_return(nvmeibc_disk_execute_gen, disk, inject_transport_error, comp, INJECT_TRANSPORT_ERROR_GEN_CMDS);
	}

	gen_cmd->disk = disk;
	gen_cmd->param.async_cb = nvmeibc_disk_local_gen_async_cb;
	switch (gen_cmd->opcode) {
	case NVMEIB_GEN_OP_JENTRY_ERASE:
	case NVMEIB_GEN_OP_GET_UUID_JOUR:
		return nvmeibs_pass_gen_to_nordda_channel(S, gen_cmd);
	case NVMEIB_GEN_OP_GET_JMDC:
	case NVMEIB_GEN_OP_BLKSET_RECOVERED:
	case NVMEIB_GEN_OP_FREE_JRNL_ENTS:
	case NVMEIB_GEN_OP_GET_EC_DB:
		return nvmeibs_pass_gen_to_server(S, gen_cmd);
	default:
		BUG();
		return 0;
	}
}

int nvmeibc_disk_locks_interlocked_cmp_exchange(void *handle, u64 addr, struct nvmeibc_d_rdma_comp *dc){
	if (dc->exchange != 0) {on_disk_hook_return(nvmeibc_disk_locks_interlocked_cmp_exchange_not_zero_, getDiskByHandle(handle), inject_transport_error, dc, INJECT_TRANSPORT_ERROR_SEND_OWNLOCK);}
	else				   {on_disk_hook_return(nvmeibc_disk_locks_interlocked_cmp_exchange_zero     , getDiskByHandle(handle), inject_transport_error, dc, INJECT_TRANSPORT_ERROR_RELE_OWNLOCK);}
	dc->opr = NVMEIBC_LOCK_CMP_AND_SWAP;
	return execute_owner_lock(getRamDiskByHandle(handle), addr, dc);
}

int nvmeibc_disk_locks_read_lock(void *handle, u64 addr, struct nvmeibc_d_rdma_comp *dc) {
	on_disk_hook_return(nvmeibc_disk_locks_read_lock, getDiskByHandle(handle), inject_transport_error, dc, INJECT_TRANSPORT_ERROR_READ_OWNLOCK);
	return execute_owner_lock(getRamDiskByHandle(handle), addr, dc);
}

int nvmeibc_disk_execute_io(struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *cmd) {
	struct nvmeib_data_reuse_buf_params *rcookie = get_rcookie_ptr(cmd);
	on_disk_hook_return(nvmeibc_disk_execute_io, disk, inject_transport_error, NULL, INJECT_TRANSPORT_ERROR_IO);
	if (rcookie->action) {
		const enum nvmeib_data_reuse_buf_enum act = rcookie->action;
		if (act == nvmeib_data_reuse_buf_SAVE) {
			if (cmd->req_id & 0x3) {	// As if sometimes transport can save the buffer
				rcookie->channel_ver= (cmd->req_id&0xFF)|1;		// Daniel: psudo random numbers
				rcookie->req_id		= 13;
			} else {}					// Sometimes transport cannot save the buffer and data will be sent over the wire twice
		} else if (act == nvmeib_data_reuse_buf_SEND_REL) {
			nvmeib_data_reuse_buf_zero(rcookie);	// As if buffer was reused and cleaned. This happens regardless of success or error of the command!
		} else {
			BUG();										// Wrong state
		}
	}

	BUG_ON(cmd->orig == NVMEIBC_DISK_IO_CMD_ORIG_NONE);

	return ramDisk_execute_io(&serverOf(disk)->ramDisk, cmd);
}

void nvmeibc_disk_reused_bb_release(struct nvmeibc_disk *disk, struct nvmeib_data_reuse_buf_params* p) {
	nvmeib_data_reuse_buf_zero(p); (void)disk;
}

int nvmeibc_disk_jmdc_read(struct nvmeibc_disk *disk, struct nvmeibc_disk_jmdc_read_comp *comp) {
	struct nvmeibc_disk_gen_cmd *gen_cmd = kzalloc(sizeof(*gen_cmd), GFP_ATOMIC);
	int rv = 0;

	if (!gen_cmd) {
		_NE_dmesg(error_nvmeibc_disk_jmdc_read, "Memory allocation error\n");
		return -ENOMEM;
	}
	nvmeibc_disk_fill_gen_op_get_jmdc(disk, comp, gen_cmd);
	rv = nvmeibc_disk_execute_gen(disk, gen_cmd);
	if (rv < 0) {
		nvmeib_buffer_free_sgl(&gen_cmd->param.jmdc_get.jmdc_sink.local);
		kfree(gen_cmd);
	 	return rv;
	}

	return 0;
}

int nvmeibc_disk_free_jrnl_ents(struct nvmeibc_disk *disk, struct nvmeibc_disk_free_jrnl_ents_comp *comp) {
	/*
	RRR: Need to call server local server gen_cmd handler directly.
	This requires to implement all possible paths of server gen_cmd in simulator.
	*/
	int rv;
	struct free_jrnl_ents_info *info = kzalloc(sizeof(*info), GFP_ATOMIC);
	if (!info) {
		_NE(error_nvmeibc_disk_free_jrnl_ents, "Free journal entries memory allocation error");
		return -ENOMEM;
	}
	rv = nvmeibc_disk_fill_free_jrnl_ents_info(disk, comp, info);
	if (rv < 0) {
		kfree(info);
		BUG_ON(rv);
	 	return rv;
	}
	rv = nvmeibc_disk_execute_gen(disk, info->gen_cmd);
	if (rv < 0) {
		kfree(info);
	 	return rv;
	}

	return 0;
}

void *nvmeibc_disk_locks_seg_locks_mem_info(struct nvmeibc_disk *disk, int seg_id){
	// Todo: connect it with the real locks.
	struct nvmeibc_disk_used_lock_segment *uls = kzalloc(sizeof(struct nvmeibc_disk_used_lock_segment), GFP_KERNEL);
	uls->mem_info 	= (void*)0x111111;			// Illegal
	uls->disk		= disk;
	uls->seg_id		= seg_id;
	return (void*)uls;
}

void nvmeibc_disk_locks_free_mem_info(void* handle){
	struct nvmeibc_disk_used_lock_segment *uls = handle;
	BUG_ON(uls->mem_info != (void*)0x111111);
	kfree(uls);
}

/*********************** Simulation of nvmeibc_disk.h  ************************/
#include "core/nvmeibc_core_common.h"
struct disk_globals { u64 dummy; };
#define __get_dg(p) ((struct disk_globals *)__get_from_params_core_globals_container(p)->dg)
void *nvmeibc_disk_create_globals(const struct nvmeibc_cinst_params_core *p) {
	struct disk_globals *dg = kzalloc(sizeof(*dg), GFP_KERNEL);
	dg->dummy = (u64)p;				// Daniel, for future use
	return dg;
}
void nvmeibc_disk_delete_globals(const struct nvmeibc_cinst_params_core *p) {
	struct disk_globals *d = __get_dg(p);
	kfree(d);
}

static int call_discover(struct nvmeibc_disk *disk){
	void *jrange_handle;
	atomic_set(&disk->paused, 0);
	atomic_set(&disk->dying,  0);
	atomic_set(&disk->restart_called, 0);
	disk->detached = false;
	serverSimulator_disk_discover(serverOf(disk), disk, nvmeibc_get_uuid(nvmeibc_cinst_get_core_p(disk)), &jrange_handle);
	if (disk->local.jrnl.valid) {
		disk->jour.rng_id = disk->local.jrnl.rng_idx;
		disk->jour.rng_gen_id = disk->local.jrnl.gen_id;
		disk->jour.rng_slba = disk->local.jrnl.rng_slba;
		disk->jour.rng_nlba = disk->local.jrnl.rng_nlba;
		disk->jour.rng_binje = disk->local.jrnl.rng_binje;
		disk->jour.rng_nblk = disk->local.jrnl.rng_nblk;
		disk->jour.max_rng_blk = disk->local.jrnl.max_rng_blk;
		disk->jour.n_ents = disk->local.jrnl.n_ents;
		disk->jour.tot_n_rng = disk->local.jrnl.tot_n_rng;
		memcpy(disk->jour.serjio_boot_id, disk->local.jrnl.serjio_boot_id, NVMEIB_GID_STR_MAX);
		if (disk->jour.rng_id != NVMEIB_EC_INVALID_JOURNAL_RANGE)
			BUG_ON(nvmeibc_jam_disk_add(disk, disk->local.jrnl.free_ents_bmp, disk->local.jrnl.jmdc, disk->local.jrnl.ent_md));
		disk->local.jrange_handle = jrange_handle;
	} else {
		nvmeibc_disk_client_journal_mark_as_no_journal(&disk->jour);
	}
	return 1;
}

void nvmeibc_disk_call_discover(struct nvmeibc_disk *disk){ (void)disk;} /* This is a request for CONT. CONT will arrive when unitest decides, so do nothing */

static ssize_t stats_fill_buf(void *priv, char *buf, size_t len) {
	#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	const struct nvmeibc_disk *disk = priv;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	ssize_t count  = 0, indent = 0;
	count += jops->start_obj(                     buf+count, len-count, NULL, indent++);
	count += jops->data_str(                      buf+count, len-count, "uuid", disk->name, !JSON_LAST_ELEM, indent);
	count += nvmeib_io_stats_to_json(disk->stats, buf+count, len-count, jiffies, jops, indent, false);
	count += jops->data_uval(                     buf+count, len-count, "overeager", (u64)atomic64_read(&disk->total_overeager), JSON_LAST_ELEM, indent);
	count += jops->end_obj(                       buf+count, len-count, JSON_LAST_ELEM, --indent);
	return count;
	#undef BUF_ADD
}

/* Slim simulator analog of alloc_dirty_bits_mem from client disk */
static int _init_dirty_bits_mem(struct nvmeibc_disk *disk) {
	INIT_LIST_HEAD(&disk->db.dirty_bits_pending_reqs);
	spin_lock_init(&disk->db.dirty_bits_spinlock);
	disk->db.dirty_bits_stopping = 0;
	_NT(trace_alloc_dirty_bits_mem, "allocating ec dirty bit pages for @DISK_NAME", disk->name);
	disk->db.len = PAGE_SIZE * 1; // no need to allcate long NVMEIBC_DIRTY_BITS_PAGES, we have short disks
	disk->db.virt = kzalloc(disk->db.len, GFP_KERNEL);
	disk->db.dirty_bits_mem.sgt.sgl = kzalloc(sizeof(*disk->db.dirty_bits_mem.sgt.sgl), GFP_KERNEL);
	if (!disk->db.virt || !disk->db.dirty_bits_mem.sgt.sgl) {
		_NE(error_alloc_dirty_bits_mem, "dirty bits memory allocation error");
		return -ENOMEM;
	}
	sg_init_one(disk->db.dirty_bits_mem.sgt.sgl, disk->db.virt, disk->db.len);
	disk->db.dirty_bits_mem.sgt.nents = 1;
	return 0;
}

// API used by volume.c
int nvmeibc_disk_create(const struct nvmeibc_cinst_params_core *p, struct nvmeibc_disk_id *disk_id, struct list_head *arnics, int num_ranges, const char *node_id){
	struct nvmeibc_disk *disk;
	struct nvmeibc_admin_rnic *arnic, *a;
	int rv = 0, i;

	extern struct nvmeibc_disk* NVMeshSystem_get_phys_disk_from_name_and_client_name(const char *name, const char* client_name);	// Extern beacuase I don't want to introduce a dependency here
	//disk = kzalloc(sizeof(*disk), GFP_KERNEL);			// Original code allocates here. We already have it from the unitest environment so extract it from the name
	disk = NVMeshSystem_get_phys_disk_from_name_and_client_name(disk_id->name, container_of(p, struct nvmeibc_cinst_params, core)->blok.dev_name.str);
	nvmeibc_cinst_get_core_p(disk) = p;

	// Since we haven't allocated the disks, some fields which should be zero are not.
	disk->version = 1;
	disk->should_pause 	= 0;
	disk->pausing		= 0;
	//atomic64_set(&disk->ops,  0);

	INIT_LIST_HEAD(&disk->volumes);
	INIT_LIST_HEAD(&disk->arnics);
	INIT_LIST_HEAD(&disk->local_nics);
	INIT_LIST_HEAD(&disk->next_arnics);
	spin_lock_init(&disk->volume_spinlock);
	spin_lock_init(&disk->stats_spinlock);
	spin_lock_init(&disk->spinlock);
	spin_lock_init(&disk->disk_conf_spinlock);
	atomic_set(&disk->n_cont_preventors, 0);
	disk->n_cont_prevents_waited_too_long = false;
	strlcpy(disk->name, disk_id->name, sizeof(disk->name)-1);
	snprintf(disk->full_name, sizeof(disk->full_name), "%.*s-%.*s", NVMEIB_HOST_NAME_LEN, utsname()->nodename, (int)sizeof(disk->name), disk->name);
	disk->disk_host[0] = '?';
	disk->access_local = true;							// As if this disk is local to client. Todo: Control it better for elect/local read optimization tests
	disk->stats = nvmeib_io_stats_create_traced(disk->name, VERB_RW_T_RECOV_BITMASK, NVMEIBC_SECTOR_SIZE);
	disk->proc_ent_stats =      nvmeib_public_proc_create(disk->name, nvmeibc_get_proc_dir_disks(p), stats_fill_buf, NULL, disk);
	disk->proc_ent_stats_json = nvmeib_public_proc_create(disk->name, nvmeibc_get_proc_dir_disks(p), stats_fill_buf, NULL, disk);
	spin_lock_init(&disk->pause_reqs_lock);
	INIT_LIST_HEAD(&disk->pause_reqs);
	disk->percpu = nvmeib_public_alloc_percpu_cacheline(struct disk_percpu);
	for_each_possible_cpu(i) {
		struct disk_percpu *pc = per_cpu_ptr(disk->percpu, i);
		pc->pause_preventers = 0;
		pc->in_transfers = 0;
	}
	#ifdef DEBUG_SUM
		atomic_set(&disk->in_transfers, 0);
	#endif
	#ifdef DEBUG_TRANSFERS
		spin_lock_init(&disk->transfer_spinlock);
		INIT_LIST_HEAD(&disk->transferring);
	#endif
	strlcpy(disk->config_node_id, node_id, sizeof(disk->config_node_id));
	disk->binje_ulp = p->binje;
	nvmeibc_add_disk(disk);							// register disk
	nvmeibc_disk_add_volume(disk, disk_id, num_ranges);
	list_for_each_entry(arnic, arnics, link) { 		// copy the admin channels
		a = kzalloc(sizeof(*a), GFP_KERNEL);
		a->ib_gid = arnic->ib_gid;
		a->pkey =  arnic->pkey;
		a->service_id = arnic->service_id;
		a->service_port = arnic->service_port;
		strlcpy(a->node_id, arnic->node_id, sizeof(a->node_id));
		list_add_tail(&a->link, &disk->arnics);
	}
	_init_dirty_bits_mem(disk);
	disk->remove_wq = wq_create("c_disk_wq");
	call_discover(disk);
	mark_disk_admin_channel_is_up(disk);					// Daniels mark
	_NT(trace_simu_disk_nvmeibc_disk_create, "Disk @DISK_NAME is connected", disk->name);
	return rv;
}

static void __clear_arnics_list(struct list_head *list) {
	struct nvmeibc_admin_rnic *arnic;
	while ((arnic = list_first_entry_or_null(list, struct nvmeibc_admin_rnic, link))) {
		list_del(&arnic->link);
		kfree(arnic);
	}
}

/* Inverse of _init_dirty_bits_mem */
static void _destroy_dirty_bits_mem(struct nvmeibc_disk *disk) {
	if (disk->db.dirty_bits_mem.sgt.sgl) {
		kfree(disk->db.dirty_bits_mem.sgt.sgl);
		disk->db.dirty_bits_mem.sgt.sgl = NULL;
	}
	if (disk->db.virt) {
		kfree(disk->db.virt);
		spin_lock_destroy(&disk->db.dirty_bits_spinlock);
	}
	disk->db.virt = NULL;
}

static void nvmeibc_disk_free(struct nvmeibc_disk *disk){
	__clear_arnics_list(&disk->arnics);
	__clear_arnics_list(&disk->next_arnics);
	// verify no volume is using the disk - the deletion of ranges shhould be when volume is removed.
	BUG_ON(!list_empty(&disk->volumes));
	if (disk->proc_ent_stats) {
		nvmeib_public_proc_remove(disk->proc_ent_stats);
		disk->proc_ent_stats = NULL;				// Not needed in the real disk because it is free()ed
	}
	if (disk->proc_ent_stats_json) {
		nvmeib_public_proc_remove(disk->proc_ent_stats_json);
		disk->proc_ent_stats_json = NULL;				// Not needed in the real disk because it is free()ed
	}
	nvmeib_io_stats_free(disk->stats);
	wq_destroy(disk->remove_wq);
	nvmeib_public_free_percpu(disk->percpu);
	_destroy_dirty_bits_mem(disk);
	//kfree(disk);								// It is not in the heap but in the unitest environment
}

static void __daniel_disk_nranges_verify(struct nvmeibc_disk *disk){
	unsigned long flags;
	BUG_ON(disk == NULL);
	spin_lock_irqsave(&disk->volume_spinlock, flags); {
		struct nvmeibc_disk_id *disk_id;
		int n = 0, n_r = 0;
		list_for_each_entry(disk_id, &disk->volumes, slink){
			if (disk_id->volume) {
				n_r += disk_id->num_ranges;
				n++;
			}
		}
		BUG_ON(n_r != disk->n_ranges);
	}
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
}

bool nvmeibc_disk_remove(struct nvmeibc_disk_id *disk_id, bool block){
	bool is_empty;
	struct nvmeibc_disk *disk;
	__daniel_disk_nranges_verify(disk_id->disk); (void)block;
	spin_lock(&disk_id->disk->volume_spinlock);
	list_del(&disk_id->slink);
	disk = disk_id->disk;
	disk->n_ranges -= disk_id->num_ranges;
	/* Note: disk_id->num_ranges is 0 when relocating segment.
	   > 0 when detachhing volume, since num_ranges-- still was not executed */
	is_empty = list_empty(&disk->volumes);
	BUG_ON(is_empty && disk->n_ranges != 0);
	BUG_ON(disk->n_ranges < 0);
	/* Note: (!is_empty && disk->n_ranges == 0) is legal when vol attach fails due to error and is immediately detacched */
	spin_unlock(&disk_id->disk->volume_spinlock);
	if (is_empty) {
		mark_disk_admin_channel_is_down(disk);					// Daniels mark
		_ND(trace_simu_disk_nvmeibc_disk_remove, "finally disk @DISK_NAME is empty", disk_id->name);
		//set_detach(disk_id->disk);
		_NT(trace_1_simu_disk_nvmeibc_disk_remove, "calling disk release from nvmeibc_disk_remove disk:@DISK_NAME", disk->name);
		disk->detached = true;
		nvmeibc_disk_start_release(disk, NVMEIBC_DISK_RELEASE_DISK_REMOVE);
		// Here: pause/cont cannot arrive anymore since admin channel is down. Must wait for pause/cont that are still scheduled on the work queue
		wq_drain(disk->remove_wq);
		nvmeibc_del_disk(disk);
		nvmeibc_disk_free(disk);
		disk_id->disk = NULL;
	}
	return false;
}

void nvmeibc_disk_add_volume(struct nvmeibc_disk *disk, struct nvmeibc_disk_id *disk_id, int num_ranges){
	NFIN;
	spin_lock(&disk->volume_spinlock);
	disk_id->disk = disk;
	list_add_tail(&disk_id->slink, &disk->volumes);
	/* disk->n_ranges += num_ranges; Do not do that!!! */ (void)num_ranges;
	spin_unlock(&disk->volume_spinlock);
	__daniel_disk_nranges_verify(disk_id->disk);
	NFOUT;
}

int nvmeibc_disk_add_work(struct nvmeibc_disk *disk, struct workqe_struct *work){ return wq_add_work(disk->remove_wq, work) ? 0 : -1; }

static inline int inst_id_of_disk(const struct nvmeibc_disk *disk) {
	//const struct nvmeibc_cinst_params *p = container_of(nvmeibc_cinst_get_core_p(disk), struct nvmeibc_cinst_params, core);
	const struct serverSimulator *srv = serverOf(disk);
	int i;
	for (i = 0; i < (int)ARRAY_SIZE(srv->client_disks); ++i) {
		if (srv->client_disks[i] == disk) {
			return i;		// != return p->index;
		}
	}
	BUG(); return -1;
}

static void complete_disk_pause(void *v) {
	struct nvmeibc_disk *disk = v;
	int rv;
	if ((rv = atomic_dec_return(&disk->n_volumes_paused)) <= 0) {
		if (rv < 0)
			_NW_dmesg(warn_simu_disk_complete_disk_pause, "Block module over triggered the disk @DISK_NAME (@DISK) pause completion (@RV)", disk->name, disk, rv);
		complete(&disk->disk_paused);
	}
	tomaSimulator_unreg_all(&serverOf(disk)->simToma, inst_id_of_disk(disk)); // ask toma to unregister all segments & then launch recovery
}

void nvmeibc_disk_pause(struct nvmeibc_disk *disk)
{
	struct nvmeibc_disk_id *disk_id;
	unsigned long flags;
	int is_ready;

	NFIN;
	BUG_ON(disk == NULL);
	spin_lock_irqsave(&disk->volume_spinlock, flags);
	disk->should_pause = true;
	BUG_ON(atomic_read(&disk->n_cont_preventors) < 0);
	atomic_inc(&disk->paused);
	list_for_each_entry(disk_id, &disk->volumes, slink)
		if (disk_id->volume) {
			nvmeibc_volume_get(disk_id->volume, NULL);
			is_ready = nvmeibc_volume_is_ready_for_pause_cont(disk_id->volume);
			if (is_ready>0)
				BUG_ON(nvmeibc_block_pause(disk_id->volume->block_dev, disk));
			else if (!is_ready) {
				/* No need to reschedule, becuase bdev tests status of disk */
			}
			nvmeibc_volume_put(disk_id->volume, NULL);
		}
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
	/* After this, no IO will be sent to the disk */
	disk->next_config_node_id[0] = 'P';			// Daniel: Using existing field for debug
	DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_RELEASE_DISK_PAUSED);
	nvmeibc_pd_pause(disk, NULL, NULL);
	NFOUT;
}

static void report_error_on_pending(struct nvmeibc_disk *disk){
	struct nvmeibc_disk_info *info = disk->info;
	struct nvmeibc_disk_command *disk_cmd;
	struct nvmeibc_disk_io_command *block_cmd;
	unsigned long flags;
	int i;

	/* report error on all pending commands */
	if (info) {
		spin_lock_irqsave(&disk->spinlock, flags);
		for (i = 0; i < DISK_PEND_PRIO_MAX; i++) {
			while ((disk_cmd = list_first_entry_or_null(&info->pending_disk_cmds[i],
				struct nvmeibc_disk_command, dcmd_link))) {
				list_del_init(&disk_cmd->dcmd_link);
				spin_unlock_irqrestore(&disk->spinlock, flags);
				if (disk_cmd->cmd_type == NVMEIBC_DISK_CMD_IO) {
					block_cmd = disk_to_block(disk_cmd);
					/* set the error */
					block_cmd->comp.comp_code = -ENXIO;
					/* update the block layer */
					nvmeibc_block_completion(&block_cmd->comp);
				} else { BUG(); }		/* Daniel: Todo, support this or prove that immpossible, Failed Gen Cmd?*/
				spin_lock_irqsave(&disk->spinlock, flags);
			}
		}
		spin_unlock_irqrestore(&disk->spinlock, flags);
	}
}

/* Debug method. pause() waits for all IO to finish transferring before it
   executes a callback of lower layer which enables cont(). This method
   replaces wait_for_completion(&disk->disk_paused) by bussy loop to allow easier debugging. Todo, remove it */
bool nvmeibc_pd_wait_for_pause_completion_debug(struct nvmeibc_disk *disk);

static bool stop_disk_io(struct nvmeibc_disk *disk){
	struct nvmeibc_disk_id *disk_id;
	int n_volumes, is_ready;
	bool allow_rediscovery;

	spin_lock(&disk->volume_spinlock);
	disk->should_pause = true;
	atomic_set(&disk->n_volumes_paused, 1);
	n_volumes = disk->n_ranges;
	if (n_volumes) {
		list_for_each_entry(disk_id, &disk->volumes, slink)
			if (disk_id->volume) {
				/* for the case we are adding a new volume just now */
				nvmeibc_volume_get(disk_id->volume, NULL);
				is_ready = nvmeibc_volume_is_ready_for_pause_cont(disk_id->volume);
				if (is_ready>0)
					BUG_ON(nvmeibc_block_pause(disk_id->volume->block_dev, disk));
				else if (!is_ready) {
					/* No need to reschedule, becuase bdev tests status of disk */
				}
				nvmeibc_volume_put(disk_id->volume, NULL);
			}
	}
	allow_rediscovery = (n_volumes > 0);
	disk->next_config_node_id[0] = 'p';			// Daniel: Using existing field for debug
	nvmeibc_pd_pause(disk, complete_disk_pause, disk);
	spin_unlock(&disk->volume_spinlock);
	report_error_on_pending(disk);
	if (true||n_volumes) {	// Always wait for completion due to existance of JAM
		//wait_for_completion_interruptible_timeout(&disk->disk_paused,NVMEIB_WAIT_BLOCK_DEV_PAUSE));
		nvmeibc_pd_wait_for_pause_completion_debug(disk);	// Verify That pause completed before we call cont
	}
	#ifdef DEBUG_SUM
	{	int sum = atomic_read(&disk->in_transfers);
		BUG_ON(sum);
	}
	#endif
	reinit_completion(&disk->disk_paused);
	allow_rediscovery = n_volumes > 0;
	return allow_rediscovery;
}

static int cont_disk_io(struct nvmeibc_disk *disk){
	struct nvmeibc_disk_id *disk_id;
	int is_ready, rv = 0;
	NFIN;
	disk->next_config_node_id[0] = 'C';			// Daniel: Using existing field for debug
	DISK_DISCOVER_STATUS(disk, NVMEIBC_DISK_RELEASE_UNKNOWN);
	nvmeibc_pd_cont(disk);
	spin_lock(&disk->volume_spinlock);
	atomic_set(&disk->paused, 0);
	disk->rediscover_timeout = 0;
	list_for_each_entry(disk_id, &disk->volumes, slink){
		if (disk_id->volume) {
			nvmeibc_volume_get(disk_id->volume, NULL);
			is_ready = nvmeibc_volume_is_ready_for_pause_cont(disk_id->volume);
			if (is_ready>0) {
				if (nvmeibc_block_cont(disk_id->volume->block_dev, disk)) {
					_NE_dmesg(error_simu_disk_cont_disk_io, "Failed to start IO on disk @DISK_NAME for volume @DEV_NAME_FULL", disk->name, disk_id->volume->full_name);
					--rv;
				}
			} else if (!is_ready) {
				/* No need to reschedule, becuase bdev tests status of disk */
			}
			nvmeibc_volume_put(disk_id->volume, NULL);
		}
	}
	spin_unlock(&disk->volume_spinlock);
	NFOUT;
	return rv;
}

extern void nvmeibc_pd_dump_transfers(struct nvmeibc_disk *disk);
bool nvmeibc_pd_wait_for_pause_completion_debug(struct nvmeibc_disk *disk){
	bool has_pause_completed = false;
	unsigned long flags;
	int i = 0, max_iters = 10000;
	int last_transfers = -1;
	BUG_ON(!disk->pausing);
	// This is the real condition verifying all IO has stopped, by waiting for block to call a completion callback whet it stopped all transferring.
	/*while (!completion_done(&disk->disk_paused)){
		udelay(10);
	}*/

	// This is a debug condition which tests the inner state of the block device (counters of IO transfers)
	while (true) {
		for (i = 0; i < max_iters; i++) {
			spin_lock_irqsave(&disk->pause_reqs_lock, flags);
			has_pause_completed = list_empty(&disk->pause_reqs);
			spin_unlock_irqrestore(&disk->pause_reqs_lock, flags);
			if (!has_pause_completed)
				udelay(10);
			else {
				// Block device finished transferring and called the callback right now. Wait a bit for it
				int j;
				for (j=0; (j<10000)&&(!completion_done(&disk->disk_paused)); j++) {
					udelay(1);
				}
				BUG_ON(!completion_done(&disk->disk_paused));
				break;
			}
		}
		if (i==max_iters) {
			int pause_preventers = 0;
			int in_transfers 	 = 0;
			for_each_possible_cpu(i) {
				pause_preventers += disk->percpu[i].pause_preventers;
				in_transfers 	 += disk->percpu[i].in_transfers;
			}
			_Emerg("Transferring = %d, Preventors=%d pausing=%d, should_pause=%d\n", in_transfers, pause_preventers, disk->pausing, disk->should_pause);
			/* Use This code to debug un-ending transfers which forbit PAUSE to occur */
			nvmeibc_pd_dump_transfers(disk);
			BUG_ON((last_transfers == in_transfers) && (last_transfers != 0));	// we expect the 'in_transfers' to decrease with every iteration BUT, since we count without locking, we might get zero as total 'in_transfers' although we havent got the completion signaled. in that case, we'll go for another round & then both previous/current 'in_transfers' would be zero
			// If core.* crashed at the above BUG_ON(),see the transfers in GDB use the following commands:
			// p *((struct nvmeibc_transfer_reason*)((char*)disk->transferring->next - 8))
			// p *(((struct nvmeibc_d_rdma_comp *)((((char*)disk->transferring->next - 8)) - 0xa0))->context)
			last_transfers = in_transfers;
		} else {
			break;
		}
	}
	if (last_transfers!=-1)
		_Emerg("-Transferring = %d, Preventors=%d pausing=%d, should_pause=%d\n", disk->percpu->in_transfers, disk->percpu->pause_preventers, disk->pausing, disk->should_pause);

	return true;
}

void rediscovery(struct nvmeibc_disk *disk){
	call_discover(disk);
	if (cont_disk_io(disk) < 0)
		_NE_dmesg(error_simu_disk_rediscovery, "Failed to restart block device IO");
}

int nvmeibc_disk_release(struct nvmeibc_disk *disk)
{
	int dying, rv = 0;
	_NT(trace_simu_disk_nvmeibc_disk_release, "Starting exceution of disk_release @DISK_NAME", disk->name);
	if (!nvmeibc_find_disk(disk)) {
		_NT(trace_1_simu_disk_nvmeibc_disk_release, "Disk @DISK was already removed", disk);
		goto out;
	}
	if ((dying = atomic_inc_return(&disk->dying)) > 1) {
		_NT(trace_2_simu_disk_nvmeibc_disk_release, "Release in process for disk @DISK_NAME (@DYING)", disk->name, dying);
		goto out;
	}

	_NT(trace_3_simu_disk_nvmeibc_disk_release, "stopping disk IO...");
	stop_disk_io(disk);
	_NT(trace_4_simu_disk_nvmeibc_disk_release, "disconnecting disk network...");
	//disconnect_disk(disk, send_disconnect);
	/* kill all admin nics */
	_NT(trace_5_simu_disk_nvmeibc_disk_release, "freeing disk resources...");
	/* earse allocated memory */
	//free_disk_rsc(disk);
	_NT(trace_6_simu_disk_nvmeibc_disk_release, "starting rediscovery...");
	atomic_set(&disk->paused, 1);
	nvmeibc_jam_disk_del(disk);
	if (disk->jour.rng_id != NVMEIB_EC_INVALID_JOURNAL_RANGE) {		// To do: Separate to a function: Server side response to pause
		serverSimulator_disk_relese(serverOf(disk), disk->jour.rng_id, disk->local.jrange_handle);
		nvmeibc_disk_client_journal_mark_as_no_journal(&disk->jour);
	}
	if (!disk->detached) {
		rediscovery(disk);
		nvmeibc_pd_cont(disk);
	}
out:
	_NT(trace_7_simu_disk_nvmeibc_disk_release, "Disk release finished"); 		// if we manage to submit a work request remember it
	return rv;
}

static void nvmeibc_disk_release_work(struct workqe_struct *work)
{
	struct disk_workq *rwork = container_of(work, struct disk_workq, work);
	struct nvmeibc_disk *disk = rwork->disk;
	kfree(rwork);
	nvmeibc_disk_release(disk);
}

int nvmeibc_disk_start_release(struct nvmeibc_disk *disk, enum nvmeibc_disk_release_reason reason)
{
	struct disk_workq *rwork;
	int restart_called;
	int rv = 0;

	/* sanity - in case channel's ptr to disk was not set */
	if (!disk) {
		_NE_dmesg(error_simu_disk_start_release_1, "Oops: disk is NULL");
		rv = -1;
		goto out;
	}

	_NT(info_simu_disk_start_release, "disk @DISK_NAME, attempt start-release with reason: @DISK_RELEASE_OP",	disk->name, reason);

	if (!(rwork = kzalloc(sizeof(*rwork), GFP_ATOMIC))) {
		_NE(error_simu_disk_start_release_2, "OOM: Fail to alloc disk-release work");
		rv = -ENOMEM;
		goto out;
	}

retry:
	if ((restart_called = atomic_inc_return(&disk->restart_called)) == 1) {
		WQ_INIT_WORK(&rwork->work, nvmeibc_disk_release_work);
		rwork->disk = disk;
		atomic_inc(&disk->shut_down_triggered);

		if ((rv = nvmeibc_disk_add_work(disk, &rwork->work)) < 0) {
			restart_called = atomic_read(&disk->restart_called);
			_NT(trace_simu_disk_start_release_1, "Failed to add disk release work for disk @DISK_NAME",
				disk->name);
			atomic_set(&disk->restart_called, 0);
			goto retry;
		}
		else
			_NT(trace_simu_disk_start_release_2, "Initiate disk @DISK_NAME release", disk->name);
	}
	else {
		_NT(trace_simu_disk_start_release_3, "disk @DISK_NAME, already called",
			disk->name);
		if (rwork)
			kfree(rwork);
	}

out:
	return rv;
}

void nvmeibc_disk_freeze(  struct nvmeibc_disk *disk){ (void)disk;	/* Daniel: Todo: copy from disk.c */ }
void nvmeibc_disk_unfreeze(struct nvmeibc_disk *disk){ (void)disk;	/* Daniel: Todo: copy from disk.c */ }
void nvmeibc_disk_block_remove(struct nvmeibc_disk_id *disk_id){(void)disk_id;}
int nvmeibc_disk_wait_for_discover(struct nvmeibc_disk_id *current_disk) {
	if ((((u64)current_disk)>>10) & 1) {
		return -ENOENT;						// Psudo randomly some disks will fail discovery (Server is down) and will rediscover in future
	}
	return 0;
}

void nvmeibc_disk_cmd_piggyback_lock_read_poison_verify(struct nvmeibc_disk_io_command *bcmd) { (void)bcmd; }

/* Copy all arnic details between two lists */
static int arnics_dup(struct list_head *target, struct list_head *source)
{
	struct nvmeibc_admin_rnic *arnic, *a;
	int n = 0;
	list_for_each_entry(arnic, source, link) {
		BUG_ON(!(a = kzalloc(sizeof(*a), GFP_KERNEL)));
		*a = *arnic;
		a->order = n++;
		list_add_tail(&a->link, target);
	}
	return 0;
}

/* 1. Sets the next configuration information (node_id/arnics)
   2. rediscovery part happen syncronously (todo: make async) */
int nvmeibc_disk_set_next_config(struct nvmeibc_disk_id *disk_id, const char *node_id){
	struct nvmeibc_disk *disk = disk_id->disk;
	int rv = 0;
	spin_lock(&disk->disk_conf_spinlock);
	_NT(trace_simu_disk_nvmeibc_disk_set_next_config, "New config node id @NODE_ID_STR will replace the existing one disk @DISK_NAME", node_id, disk->name);
	strlcpy(disk->next_config_node_id, node_id, sizeof(disk->next_config_node_id));
	__clear_arnics_list(&disk->next_arnics);
	rv = arnics_dup(&disk->next_arnics, nvmeibc_disk_id_to_nics(disk_id));
	disk->new_config_ready = true;

	// Simulate rediscovery happens in-line, finds and uses first nic. The rest go to another list
	strlcpy(disk->config_node_id, disk->next_config_node_id, sizeof(disk->config_node_id));
	__clear_arnics_list(&disk->arnics);
	list_splice_init(&disk->next_arnics, &disk->arnics);
	disk->new_config_ready = false;
	spin_unlock(&disk->disk_conf_spinlock);
	return rv;
}

void nvmeibc_disk_volumes_get(struct nvmeibc_disk *disk, unsigned long *flags) {
	if (flags)  spin_lock_irqsave(		&disk->volume_spinlock, *flags);
	else  		spin_lock(				&disk->volume_spinlock);
}

void nvmeibc_disk_volumes_put(struct nvmeibc_disk *disk, unsigned long *flags) {
	if (flags)  spin_unlock_irqrestore(	&disk->volume_spinlock, *flags);
	else  		spin_unlock(			&disk->volume_spinlock);
}

/********************* Simulator of toma communication ***********************/
static bool __does_node_and_nic_exist(struct nvmeibc_disk *disk) {  // Checking if toma simulator disk is accessed correctly
	struct nvmeibc_admin_rnic *arnic		 = NULL;
	struct serverSimulator* srvr		 = serverOf(disk);
	bool rv = true;
	spin_lock(&disk->disk_conf_spinlock);
	arnic = list_first_entry_or_null(&disk->arnics, struct nvmeibc_admin_rnic, link);
	if (!arnic || strcmp(srvr->hardware->node_id, arnic->node_id)) { // Either disk has no arnics or disk resides on a server that is not the same as the arnic it uses
		_NT(trace_simu_disk_does_node_and_nic_exist, "disk @DISK_NAME: Ignoring msg, due to arnic mismatch server node=@NODE, arnic node=@NODE", disk->name, srvr->hardware->node_id, (arnic ? arnic->node_id : "NULL"));
		rv = false;
	} else if (strcmp(srvr->hardware->disk_name, disk->name)) {   // Disk resides on a different server than this toma
		_NT(trace_1_simu_disk_does_node_and_nic_exist, "disk @DISK_NAME: Ignoring msg, due to server mismatch (disk @DISK_NAME)", disk->name, srvr->hardware->disk_name);
		rv = false;
	}
	spin_unlock(&disk->disk_conf_spinlock);
	return rv;
}

int nvmeibc_disk_toma_send(struct nvmeibc_disk *disk, u64 handle, struct nvmeibc_disk_toma_send_params *params){
	if (!__does_node_and_nic_exist(disk))
		return 0; 	// 0 = Client thinks msg was sent. Is this correct?

	return nvmeibs_toma_intercept_msg(serverOf(disk), handle, params);
}

int nvmeibc_disk_subscribe_toma_service(struct nvmeibc_disk *disk, u64 handle, struct nvmeibc_disk_subscription_params *params){
	struct tomaSimulator* simToma = &serverOf(disk)->simToma;
	int rv = 0;
	if (nvmeibc_disk_get_status(disk) != d_online) {
		_NI(trace_simu_disk_nvmeibc_disk_subscribe_toma_service, "Deferring SUBSCRIBE disk=@DISK handle=@HANDLE", disk, handle);
		rv = -EAGAIN; /* This usecase is not an error for upper software layer */
	}
	return tomaSimulator_subscribe_seg(simToma, handle, params, &nvmeibc_get_uuid(nvmeibc_cinst_get_core_p(disk))->b[0], rv);
}

int nvmeibc_disk_unsubscribe_toma_service(struct nvmeibc_disk *disk, u64 handle) {
	return tomaSimulator_unsubscri_seg(&serverOf(disk)->simToma, handle);
}

int nvmeibc_disk_dbg_please_kill_yourself(struct nvmeibc_disk *disk, void (*cb)(void *), void *ctx, int rsc_id, u64 dlba) {
	(void)disk; (void)cb; (void)rsc_id; (void)dlba; (void)ctx;
	BUG();
	return 0;
}

bool nvmeibc_disk_do_512b_sub_block_x_supported(const struct nvmeibc_disk* disk) {
	(void)disk; return true; // Todo: support toggling this for unitest
}

int nvmeibc_disk_notify_coremask_update(struct nvmeibc_disk *disk) {
	(void)disk;
	return 0;
}

u64 nvmeibc_disk_version_get(struct nvmeibc_disk *disk)
{
	return disk->version;
}

/*****************************************************************************/
// EOF.

