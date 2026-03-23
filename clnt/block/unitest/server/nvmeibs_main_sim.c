/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

// For documentation, see Header in H file
/*****************************************************************************/
#include "nvmeibs_main_sim.h"
#include "nvmeibc_block.h"
#include "nvmeibs_serjio.h"
#include "nvmeibs_serjio_gen_cmd_handlers.h"
#include "nvmeibc_jam.h"
#include "nvmeibs_serjio_sim_access.h"
#include "nvmeibs_nic_dma_atomics.h"			// Send msges to JAM
#include "uni_framework/range_algorithms.h"
#include "nvmeibc_ib_nordda_channel_sim_shared.h"
#include "nvmeibs_nordda_sim_shared.h"
#include "nvmesh_sim.h"
#include "nvmeibc_icore_ops.h"
#include "toma/nvmeibr_disk_metadata_sim.h"
#include "nvmeibs_srv_toma_messages.h"
#include "nvmeibs_serjio_gpt.h"
#include "nvmeibs_memmgr_metrics.h"
#include "nvmeibc_pausable.h"

void *sim_kcalloc(size_t n, size_t size, gfp_t flags);

struct proc_dir_entry *nvmeibs_proc_dir = NULL;

/****** Pseudo-client, used by unitesting: Todo, move to a dedicated file *****/
struct t_other_clnt {
	uuid_be uuid;
	const char urn_uuid[URN_UUID_STR_LENGTH + 4];
	struct nvmeib_jrange_rsp J[NVMESH_N_PHYS_DISKS]; // Information about curent journal range
	union jblock_md jmdc[NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE * NVMESH_N_PHYS_DISKS];
	u32 cid[NVMESH_N_PHYS_DISKS]; 				 // CID when connected to each disk
} other_clnt = {
		.uuid = {.b = {'O','t','h','e','r',' ','C','l','n','t'}},	// 4f 74 68 65 72 20 43 6c 6e 74 00 00 00 00
		.urn_uuid = "6568744f-2072-6c43-6e74-000000000000",
		.cid={0}};

const uuid_be *SIMULATOR_OTHER_CLIENT__get_uuid(void) {return &other_clnt.uuid;}
const char *SIMULATOR_OTHER_CLIENT__get_urn_uuid(void) {return other_clnt.urn_uuid;}
void SIMULATOR_OTHER_CLIENT_set_jentry(u32 srv, int ei, bool abandon) {
	BUG_ON(srv >= ARRAY_SIZE(other_clnt.J));
	if (abandon)
		clear_bit(  ei, (void *)&other_clnt.J[srv].free_ents_bmp);
	else
		set_bit(ei, (void *)&other_clnt.J[srv].free_ents_bmp);
}

const uuid_be alien_clnt_uuid = {.b = {'A','l','i','e','n',' ','C','l','n','t'}};
const uuid_be *SIMULATOR_ALIEN_CLIENT__get_uuid(void) {return &alien_clnt_uuid;}

/************************ Simulator of nvmeibs_no_rdda.c/.h *******************/

//the simulator does not have nvmeibs_disk_info definition, so we "emulate" it, using serverSimulator struct.
struct nvmeibs_disk_info;
static inline struct nvmeibs_disk_info * as_nvmeibs_disk_info(struct serverSimulator* srv){
	return &srv->ramDisk.server_disk.di;
}

int nvmeibs_find_jri_by_uuid(struct serverSimulator* S, const uuid_be* cuuid) {
	return nvmeibs_serjio_find_jrange_index_by_client_uuid(as_nvmeibs_disk_info(S), *cuuid);
}

int nvmeibs_get_jri_by_uuid(struct serverSimulator* S, const uuid_be* cuuid) {
	int rv = nvmeibs_serjio_find_jrange_index_by_client_uuid(as_nvmeibs_disk_info(S), *cuuid);
	BUG_ON(!nvmeib_uuid_cmp(*cuuid, NULL_UUID_BE) || (rv < 0));
	return rv;
}

bool nvmeibs_is_other_client(const uuid_be cuuid){
	return !memcmp(&cuuid, &other_clnt.uuid, sizeof(cuuid));
}

void serverSimulator_serjio_drain_wq(struct serverSimulator* srv){
	nvmeibs_serjio_drain_wq(as_nvmeibs_disk_info(srv));
}

static inline u32 __gen_cid(struct ramDiskSimulator *D) {		// Generate unique ID (in valid range)
	const u32 mask_12bits = 0xFFF;								// 4096 clients
	D->c.id_gen = ((D->c.id_gen + 1) & mask_12bits);
	return D->c.id_gen;
}

/************************ Simulator of nvmeibs_toma.c/.h *******************/
int nvmeibs_toma_to_serjio_clean_range(struct serverSimulator *S, const char *seg_uuid,
									   const u64 start_lba, const u64 end_lba, bool seg_delete) {
	int rv = nvmeibs_serjio_clean_journal_for_disk_range(as_nvmeibs_disk_info(S), seg_uuid, start_lba, end_lba, seg_delete);
	if (!rv || rv == -EINPROGRESS) { // Wait for completion
		wait_for_completion(&S->simToma.serjio_done);
	} else BUG_ON(rv);
	return rv;
}

int nvmeibs_toma_report_event_serjio_disk_range_cleaned(void *di, const char *seg_id) {
	struct serverSimulator* S = (struct serverSimulator*)di;
	(void)seg_id;
	complete(&S->simToma.serjio_done);
	return 0;
}

/************************ Simulator of nvmeibs_main.c/.h *******************/
static void __remove_cid_clients_cb(void *_comp)
{
	complete((struct completion *)_comp);
}

int nvmeibs_remove_cid_clients(u64 cid, enum nvmeibs_logout_reason reason) {
	extern struct NVMeshSystem* g_sys; // Todo: Ugly, rmeove
	int disk;
	struct nvmeibc_disk *client_disk = NULL;
	struct clientSimulator *client;
	int rv;
	(void)reason;

	for_each_active_client(&g_sys->clients[0], client) {
		for (disk = 0; disk < NVMESH_N_PHYS_DISKS; ++disk) {
			if (client->physDiscs[disk].cid == cid) {
				client_disk = &client->physDiscs[disk];
				break;
			}
		}
	}
	if (client_disk) {
		DECLARE_COMPLETION_ONSTACK(comp);
		client_disk->should_pause = true;	// Simulate as if we are in pause
		_NT(trace_1_nvmeibs_remove_cid_clients, "Pausing disk @DISK_ID_NAME of Client @CID_LLONG\n",
			((client_disk)->base.ops.get_name(&((client_disk))->base)), cid);
		nvmeibc_pd_pause(client_disk, __remove_cid_clients_cb, &comp);
		wait_for_completion(&comp); // Wait for rA exexution (# of pending ios == 2)
		_NT(trace_2_nvmeibs_remove_cid_clients, "Paused disk @DISK_ID_NAME of Client @CID_LLONG\n",
			((client_disk)->base.ops.get_name(&((client_disk))->base)), cid);
		nvmeibc_disk_start_release(client_disk, NVMEIBC_DISK_RELEASE_UNKNOWN);
		rv = 0;
	} else {
		_NE_dmesg(error_1_nvmeibs_remove_cid_clients, "CID @CID_LLONG not found\n", cid);
		rv = -ENOENT;
	}

	return rv;
}

extern uint nvmeibc_jentry_num_blocks;		// Todo: No!!!! Remove this and use (struct nvmeibc_cinst_params_blk).jentry_num_blocks generated by nvmeibc_block_dp_fill_cinst_params_from_module_params()
int __alloc_journal_range_when_ready(struct nvmeibs_disk_info *di, u32 client_id, uuid_be client_uuid,
                                      const char *client_host, const struct nvmeib_jrange_cache *jrc,
                                      struct nvmeib_jrange_rsp *local_rsp) {
	static const unsigned long SERJIO_JOURNAL_ALLOC_RETRY_TIMEOUT = 3000000UL; /* In jiffies */
	unsigned long jstart = jiffies;
	int rv;
	nvmeibs_serjio_wait_serjio_ready(di);
	do {
		if (jrc->rng_binje != 0)
			WARN_ON(jrc->rng_binje != nvmeibc_jentry_num_blocks);			// If client requests specific binje then it must be corect
		rv = nvmeibs_serjio_alloc_journal_range(di, client_id, client_uuid, client_host, nvmeibc_jentry_num_blocks, jrc, local_rsp);
		if (rv == -EAGAIN)
			usleep(10);
	} while (rv == -EAGAIN && jiffies - jstart < SERJIO_JOURNAL_ALLOC_RETRY_TIMEOUT);
	return rv;
}

void __give_jour_to_clnt(struct serverSimulator* S, u32* rv_cid, uuid_be *cuuid, struct nvmeib_jrange_rsp *rsp, void **jrange_handle_ptr) {
	struct nvmeibs_disk_info* di = as_nvmeibs_disk_info(S);
	int rv;
	struct nvmeib_jrange_cache jrc = {0};
	if (nvmeibs_is_other_client(*cuuid)){
		jrc.rng_id = NVMEIB_EC_INVALID_JOURNAL_RANGE;
	} else{
		jrc.rng_id = S->disk->jrc.jour.rng_id;
		jrc.gen_id = S->disk->jrc.jour.rng_gen_id;
		BUILD_BUG_ON(ARRAY_SIZE(jrc.free_bmp) != ARRAY_SIZE(S->disk->jrc.free_bitmap));
		array_copy(jrc.free_bmp, S->disk->jrc.free_bitmap);
		BUILD_BUG_ON(ARRAY_SIZE(jrc.ent_md) != ARRAY_SIZE(S->disk->jrc.ent_md));
		array_copy(jrc.ent_md, S->disk->jrc.ent_md);
		array_copy(jrc.serjio_boot_id, S->disk->jrc.jour.serjio_boot_id);
		jrc.rng_binje = S->disk->jrc.jour.rng_binje;
	}
	*rv_cid = __gen_cid(&S->ramDisk);
	_NT(trace_nvmeibs__give_jour_to_clnt, "assigned cid @CID to client @CLIENT_UUID for disk @DISK_ID_STR",
		*rv_cid, cuuid, as_nvmeibs_disk_info(S)->disk_id);

	rv = __alloc_journal_range_when_ready(di, *rv_cid, *cuuid, nvmeib_get_utsname_nodename(), &jrc, rsp);
	BUG_ON(rv < 0); // Including (-EALREADY), If (rv == -EAGAIN) then unitest environment has not waited for serjio to boot.
	if (jrange_handle_ptr) {
		*jrange_handle_ptr = nvmeibs_serjio_get_jrange_handle(di, *rv_cid, rv, NULL, NULL);
		BUG_ON(IS_ERR_OR_NULL(*jrange_handle_ptr));
	}
}

int nvmeibs_nordda_jentry_erase(struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gen_cmd) {
	struct ramDiskSimulator *ram = &S->ramDisk;
	struct block_inject_ptrs bptrs = serverSimulator_get_block_inject_ptrs(S, ram->committed_addr.block, gen_cmd->param.je.rng_idx, gen_cmd->param.je.ent_erase.ent_idx, 0);
	int rv;
	_NW(warn_nvmeibs_nordda_jentry_erase, "erasing jentry: disk=@DISK_ID jri=@JRI idx=@IDX", ram->uniqueID, gen_cmd->param.je.rng_idx, gen_cmd->param.je.ent_erase.ent_idx);

	/**
	 * RRRR:
	 * Currenttly we do not really have channel simulated, and no client from server point of view.
	 * I use rng_id from gen command, thought it should come from nvmeibs_client object and used for
	 * input validation. Maybe change that later.
	 */
	if ((rv = nvmeibs_serjio_erase_jam_ent(S->disk->local.jrange_handle, gen_cmd->param.je.rng_binje,
		gen_cmd->param.je.rng_gen_id, 1, &gen_cmd->param.je.ent_erase, JAM_ENT_ERASE_REASON_FAILED_WRITE)))
		return rv;
	spin_lock(&ram->cmpxchg_lock);
	if (ram->state & ramDisk_down) {
		spin_unlock(&ram->cmpxchg_lock);
		gen_cmd->comp_code = -NCL_STATUS_FAIL_NO_COMP;					// There will be no callback
		_NW(warn1_nvmeibs_nordda_jentry_erase, "locks channel paused, erasing jentry request canceled; disk=@DISK_ID jri=@JRI idx=@IDX canceled", ram->uniqueID, gen_cmd->param.je.rng_idx, gen_cmd->param.je.ent_erase.ent_idx);
		return -EIO;
	}

	bptrs.ram.jmdc->raw = nvmeib_jmd_unused_entry_val.raw;
	memset(bptrs.jrnl, 0, NVMEIBC_SECTOR_SIZE);
	bptrs.jmd->raw = nvmeib_jmd_unused_entry_val.raw;
	spin_unlock(&ram->cmpxchg_lock);
	return 0;
}

int nvmeibs_nordda_get_jrng_by_uuid(struct serverSimulator *S, struct nvmeibc_disk_gen_cmd *gc) {
	struct handle_get_uuid_jour_cb_param param = {
		.gen_param = &gc->param,
		.gen_rsp = &gc->rsp,
	};
	int rv;

	/* WTF is this piece of code and why we need it:
	 * The idea is to use nvmeibs_gen_cmd_handle_get_uuid_jour inside simulator that is the same piece
	 * of code that runs on server. When runnig on server, this function assumes the data passes
	 * through rdma, which is not correct for simulator. The result is that:
	 * 1. All data generated by nvmeibs_gen_cmd_handle_get_uuid_jour is BE on server, but client expects LE.
	 *    Hence, in simulator we've got to make sure it is correctly converted back to LE.
	 * 2. JMDC read is 4K aligned, while client supplied buffers are not guaranteed to be aligned or large
	 *    enough to contain aligned data. Hence we have to first allocate a buffer large enough, then
	 *    copy aligned info to client's buffer.
	 */

	/* Call server side code */
	rv = nvmeibs_gen_cmd_handle_get_uuid_jour(as_nvmeibs_disk_info(S), &param);

	return rv;
}

void nvmeibs_pass_loser_to_serjio(struct serverSimulator *S, struct nvmeibs_lost_srv_resource_payload *p) {
	int rv = nvmeibs_serjio_abnd_jrnl_ents(as_nvmeibs_disk_info(S), ((S->disk)->base.ops.get_journal(&((S->disk))->base))->rng_id, p->bmp, p->gen_ids);
	if (rv && rv != -EINVAL){
		//TODO LKJ
		//Hack: today jri may be released in 2 ways:
		//* by client (volume detach for example)
		//* by server - on client disconnect
		//The simulator releases JRI from disk_release function (as is client release range in a good way)
		//There are few problems with this solution:
		//* detach (or any other good way disconnect) - send loser data to the server,
		//  which must be processed by serjio first and then by toma; BUT client does not wait for this process
		//  to finish and continues to disk release. "disk release" is just a function call, which comes before the sent loser message
		//* disconnect - does not release the jri (all of them).
		BUG_ON(rv);
	}
}

void nvmeibs_nordda_jour_entry_do(struct serverSimulator *S, u32 jri, u32 ei, const char* action) {
	_ND(trace_main_sim_nvmeibs_nordda_jour_entry_do, "Disk @RAMDISK_UNIQUEID: Jour={@JRI,@EI}=@ACTION_STR", S->ramDisk.uniqueID, jri, ei, action);
	nvmeibs_serjio_entry_do(as_nvmeibs_disk_info(S), jri, ei, action);
	if (action[0] == 'A' || action[0] == 'U') {		// If abandon, do it in serjio and jams
		if (other_clnt.J[S->ramDisk.uniqueID].rng_idx == jri)
			SIMULATOR_OTHER_CLIENT_set_jentry(S->ramDisk.uniqueID, ei, true);
		else
			nvmeibc_jam_simu_process_entry_event(S->disk, ei, NVMEIBC_JIDX_EVT_ABANDON);
	} else {}	// Deliberately do nothing, serjio will notify JAMs
}

void nvmeibs_nordda_verify_no_abandjour(struct serverSimulator *S) {
	nvmeibs_serjio_verify_no_abandoned_entries(as_nvmeibs_disk_info(S));
#if 0
	other_clnt.J[S->ramDisk.uniqueID].abmp = 0;			// EC-1478: When serjio sends msg to JAM, remove this line
	BUG_ON(other_clnt.J[S->ramDisk.uniqueID].abmp);
#endif
}

int nvmeibs_nordda_count_abandjour_in_jri(struct serverSimulator *S, u32 jri) {
	return __builtin_popcount(nvmeibs_nordda_get_abandjour_in_jri(S, jri));
}

u32 nvmeibs_nordda_get_abandjour_in_jri(struct serverSimulator *S, u32 jri) {
	return nvmeibs_serjio_get_abandoned_bmp_by_jri(as_nvmeibs_disk_info(S), jri);
}

void nvmeibs_nordda_process_jmd_free_abandoned(struct serverSimulator *S, u64 client_id, u32 rng_num, binje_t rng_binje, u64 rng_gen_id,
											   u32 *abnd_free_bitmap, struct nvmeib_jrnl_ent_md *ent_md) {
	struct nvmeibc_nic_rspreq *rr = sim_kzalloc(sizeof(*rr), GFP_KERNEL);
	struct volume_server_req *req = &rr->req;
	req->hdr.tag = 0x1131;											// Todo: What to set here?
	req->hdr.opcode = 0x11;											// Todo: What to set here?
	req->j_req.base.rng_num = cpu_to_be32(rng_num);
	req->j_req.base.rng_gen_id = cpu_to_be64(rng_gen_id);
	for(u32 i = 0; i < ARRAY_SIZE(req->j_req.base.abnd_free_bitmap); ++i){
		req->j_req.base.abnd_free_bitmap[i] = cpu_to_be32(abnd_free_bitmap[i]);
	}
	for(u32 j = 0; j < ARRAY_SIZE(req->j_req.base.free_ent_md); ++j){
		req->j_req.base.free_ent_md[j] = ent_md[j];
	}
	req->j_req.ext1.binje = binje_to_be(rng_binje);

	if (other_clnt.J[S->ramDisk.uniqueID].rng_idx == rng_num) {
		req->opcode = 0xBC;											// Todo: Invent a smarter way for encoding other client
		BUG_ON(other_clnt.cid[S->ramDisk.uniqueID] != client_id);
		for (u32 k = 0; k < (int)ARRAY_SIZE(other_clnt.J[S->ramDisk.uniqueID].free_ents_bmp); k++)
			other_clnt.J[S->ramDisk.uniqueID].free_ents_bmp[k] |= abnd_free_bitmap[k];	// Other clients JAM responds inline
	} else {
		req->opcode = NVMEIBS_JAM_ABND2FREE;
		BUG_ON(S->disk->cid != client_id);
	}
	nvmeibc_nic_send_jam_free_abandoned(S, rr);
}

void serverSimulator_disk_discover_by_unitest(struct serverSimulator* S){
	const u32 srv = S->ramDisk.uniqueID;
	struct nvmeib_jrange_rsp *p = &other_clnt.J[srv];
	p->jmdc = &other_clnt.jmdc[srv * NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE];
	__give_jour_to_clnt(S, &other_clnt.cid[srv], &other_clnt.uuid, p, NULL);
	BUG_ON(p->rng_idx != (u32)(NVMEIB_EC_MAX_JOURNAL_RANGES-1));	// Unitest environment reserved the last jri to itself
}

#define SERJIO_USED_ON_4K_BLOCKS 	(NVMEIBC_SECTOR_SHIFT == 12)
void serverSimulator_disk_discover(struct serverSimulator *S, struct nvmeibc_disk* disk, uuid_be* cuuid, void **jrange_handle_ptr) {
	struct ramDiskSimulator *D = &S->ramDisk;
	u32 cid = 0;

	disk->local.jrnl.valid = ( SERJIO_USED_ON_4K_BLOCKS || ramDiskSimulator_has_metadata(D));				// Todo: In future, call this only if EC is relevant
	if (disk->local.jrnl.valid) {
		if (!disk->local.jrnl.jmdc) {
			if (!(disk->local.jrnl.jmdc = sim_kcalloc(NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE, sizeof(*disk->local.jrnl.jmdc), GFP_KERNEL))) {
				_NE(tnsdd_0, "OOM Error\n");
			}
		}
		__give_jour_to_clnt(S, &cid, cuuid, &disk->local.jrnl, jrange_handle_ptr);
	} else {
		disk->local.jrnl.rng_idx = NVMEIB_EC_INVALID_JOURNAL_RANGE;
	}
	disk->sector_shift           = D->sector_shift;
	disk->max_request_size_bytes = D->max_dma_size;		// As if after discovery server told client the size of the disk
	disk->cid               	 = cid;
	disk->md_size				 = (D->md_size >> (NVMEIBC_SECTOR_SHIFT - ((disk)->base.ops.get_sector_shift(&((disk))->base))));	// md[bytes] per each physical sector

	_NT(tnsdd_1, "got cid @CID for client @CLIENT_UUID for client disk @DISK_ID_STR",
		cid, cuuid, ((disk)->base.ops.get_name(&((disk))->base)));

	tomaSimulator_onClntDiscovery(&S->simToma, cuuid, disk->local.jrnl.rng_idx);
}

void serverSimulator_disk_relese(struct serverSimulator *S, u32 jri, void *jrange_handle) {
		int rv;
		nvmeibs_serjio_put_jrange_handle(jrange_handle, NULL);
		rv = SERJIO_USED_ON_4K_BLOCKS ? nvmeibs_serjio_return_journal_range(as_nvmeibs_disk_info(S), jri) : 0;
		tomaSimulator_onClntDiskRelese(&S->simToma, jri);
		if (rv == -EINPROGRESS)
			nvmeibs_serjio_drain_wq(as_nvmeibs_disk_info(S));
		else
			WARN(rv, "serjio failed rv=%d, jri=%u\n", rv, jri);
}

char *nvmeibs_nvme_get_model(struct device_data *d)
{
	(void)d;
	return "Dummy disk model";
}

char *nvmeibs_nvme_get_native_serial(struct device_data *d)
{
	(void)d;
	return "Dummy disk native_serial";
}

/*********************** Server internal representation **********************/
void serverSimulator_init_hardware(struct serverSimulator* S, int unique_id, struct mdb_target_conf* hardware){
	S->hardware  = hardware; //partially initialized
	BUG_ON(ramDiskSimulator_init(&S->ramDisk, unique_id) < 0);
}

void serverSimulator_init_serjio(struct serverSimulator* S){
	if (SERJIO_USED_ON_4K_BLOCKS) {
		struct nvmeibs_disk_info* di = as_nvmeibs_disk_info(S);
		bool has_md = ramDiskSimulator_has_metadata(&S->ramDisk);
		ramDiskSimulator_format_metadata(&S->ramDisk, true);
		ramDiskSimulator_wipeMD_jour(    &S->ramDisk, nvmeib_disk_init_md_max);
		ramDiskSimulator_wipeMD_serjioDB(&S->ramDisk, nvmeib_disk_init_md_max);
		BUG_ON(nvmeibs_serjio_disk_init(di) < 0);
		nvmeibs_serjio_wait_serjio_ready(di);						// Make sure serjio finished its io and kill its thread
		S->jmdc = nvmeibs_serjio_get_jmdc_ptr(di);
		ramDiskSimulator_format_metadata(&S->ramDisk, has_md);
		serverSimulator_disk_discover_by_unitest(S);
	}
}

void serverSimulator_destroy(struct serverSimulator* S) {
	const u32 srv = S->ramDisk.uniqueID;
	serverSimulator_disk_relese(S, other_clnt.J[srv].rng_idx, NULL);
	nvmeibs_serjio_disk_free(as_nvmeibs_disk_info(S));
	ramDiskSimulator_destroy(&S->ramDisk);
}

void serverSimulator_disconnect(struct serverSimulator*_this){
	ramDiskSimulator_disconnect(&_this->ramDisk);
	tomaSimulator_disconnect(&_this->simToma);
}

void serverSimulator_re_connect(struct serverSimulator*_this){
	ramDiskSimulator_reconnect(&_this->ramDisk);
	tomaSimulator_reconnect(&_this->simToma);
}

void serverSimulator_notify_new_disk_sgmnts(struct serverSimulator* self){
	int rv = 0;
	struct gpt_header gpt_hdr = {0};
	int n_gpt_ents;
	struct gpt_entry gpt_ents[GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES];
	int gpt_upd_type;

	memset(&gpt_ents, 0, sizeof(gpt_ents));
	/* Need to send the following GPT update events to SERJIO
	 * Update. (primary == false, gpt_update == MAIN_GPT_UPDATE_HEADER_PRE_UPDATE)
	 * Update done. (primary == false, gpt_update == MAIN_GPT_UPDATE_HEADER_PRE_UPDATE)
	 * Update. (primary == false, gpt_update == MAIN_GPT_UPDATE_HEADER_POST_UPDATE)
	 * Update done. (primary == false, gpt_update == MAIN_GPT_UPDATE_HEADER_POST_UPDATE)
	 * Update. (primary == true, gpt_update == MAIN_GPT_UPDATE_HEADER_PRE_UPDATE)
	 * Update done. (primary == true, gpt_update == MAIN_GPT_UPDATE_HEADER_PRE_UPDATE)
	 * Update. (primary == true, gpt_update == MAIN_GPT_UPDATE_HEADER_POST_UPDATE)
	 * Update done. (primary == true, gpt_update == MAIN_GPT_UPDATE_HEADER_POST_UPDATE)
	 */

	for (gpt_upd_type = 0; gpt_upd_type < 4; gpt_upd_type++) {
		bool primary = gpt_upd_type >= 2;
		enum nvmeib_main_gpt_update_flags gpt_update =
			(gpt_upd_type % 2 == 0) ? MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE : MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE;

		nvmeibr_disk_metadata_store_gpt(self, &gpt_hdr, primary);
		n_gpt_ents = le32_to_cpu(gpt_hdr.num_partition_entries);
		BUG_ON(n_gpt_ents > (int)ARRAY_SIZE(gpt_ents));
		nvmeibr_disk_metadata_store_entries(self, gpt_ents, 0, GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES,
						    GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES, NULL);

		rv = nvmeibs_serjio_gpt_update(as_nvmeibs_disk_info(self), primary, gpt_update, 0, &gpt_hdr, sizeof(gpt_hdr),
										gpt_ents, n_gpt_ents * sizeof(gpt_ents[0]));
		BUG_ON(rv);
		rv = nvmeibs_serjio_gpt_update_done(as_nvmeibs_disk_info(self), primary, gpt_update, 0);
		BUG_ON(rv);
	}

	nvmeibs_serjio_wait_serjio_ready(as_nvmeibs_disk_info(self));
}

struct block_inject_ptrs
serverSimulator_get_block_inject_ptrs(struct serverSimulator *self, u64 addr, s32 jri, s32 jentry, u32 height){// EC-6136
	struct tomaSimulator *toma = &self->simToma;
	struct ramDiskSimulator *ssd = &self->ramDisk;
	const u64 bsinfo_index = COMMITTED_ADDR_AS(ssd, addr, 4KB, LOCK);

	u8* jrnl = NULL;
	union jblock_md *jmd = NULL;
	union jblock_md *jmdc = NULL;

	if (0 <= jri){
		const u64 jblock_offset_rel = nvmeibs_jmdc_ind_of(jri, jentry, nvmeibc_jentry_num_blocks) + height;
		const u64 jblock_offset_abs = jblock_offset_rel + ssd->committed_addr.block;
		jmd = ramDiskSimulator_get_metadataptr_jblk(ssd, jblock_offset_abs);
		jrnl = nvmeibs_jdisk_ptr(ssd, jri, jentry, height, nvmeibc_jentry_num_blocks);
		jmdc = &self->jmdc[jblock_offset_rel];
	}

	return (struct block_inject_ptrs){
		.self = self,
		.dlba=addr,
		.data=&(ssd->mem[COMMITTED_ADDR_AS(ssd, addr, SECTOR, BYTE)]),
		.dmd=ramDiskSimulator_get_metadataptr(ssd, addr),
		.jrnl = jrnl,
		.jmd = jmd,
		.jri = jri,
		.jentry = jentry,
		.ram = {
			.lock=&(ssd->locks[bsinfo_index]),
			.txid=&(ssd->TxIDs[bsinfo_index]),
			.dbits=&(ssd->dbits[bsinfo_index]),
			.jmdc = jmdc
		},
		.toma_stale_lock = &toma->stale_locks[bsinfo_index],
	};
}

/*****************************************************************************/
// EOF.

