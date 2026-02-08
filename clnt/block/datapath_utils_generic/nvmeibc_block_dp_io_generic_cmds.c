#include "nvmeibc_block_dp_io_generic_cmds.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "nvmeib.h"
#include "nvmeib_types.h"
#include "nvmeibc_block.h"
#include "nvmeibc_block_dp_io_req_rel_locks.h"
#include "nvmeibc_icore_ops.h"
#include "nvmeibc_block_dp_dbg_tools.h"
#include "block/nvmeibc_block_common.h"
#include "../datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_profiling_disk_stages.h"
#include "nvmeibc_block_dp_block_md.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeib_nvme.h"
// For mssa
#include "block/datapath_ec/nvmeibc_block_dp_ec.h"
#include "common/nvmeib_str.h"
#include "nvmeibc_error_tags.h"
#include "nvmeibc_io_pet.h"
#include "compat/kr_incs_sgl.h"
#include "common/nvmeib_scatterlist_iter.h"

/******************************************************************************/
uint nvmeibc_jentry_num_blocks = 16;						// Todo, rename internally to binje
module_param(nvmeibc_jentry_num_blocks, uint, 0644);
MODULE_PARM_DESC(nvmeibc_jentry_num_blocks, "Length of erasure coding journal, in blocks. For erasure coding volumes, increasing this means fewer parallel write IO operations, but more efficient large writes. It is highly recommended to increase for use cases with large writes. Range is 1 to 16.");

bool nvmeibc_ec_reuse_req = true;
module_param_named(ec_reuse_req, nvmeibc_ec_reuse_req, bool, 0644);
MODULE_PARM_DESC(ec_reuse_req, "Enable reusing feature for requests (EC). This parameter was added to facilitate disabling this reuse as a potential optimization for NVMesh in DPU mode.");

NVMEIBC_MEMMGR_METRIC(dp_commands, "component=raid.io.commands");

static void __verify_dirtybits_union_bits(void)
{
	union nvmeibc_dbits_entry t = {};
	t.bsmod.mod_marker = 0xd;
	WARN(!t.bsmod.is_d0_convict, "EC Ditry Bits Marker is not aligned, d0 convict not marked correctly");
	WARN( t.bsmod.is_d1_convict, "EC Ditry Bits Marker is not aligned, d1 convict not marked correctly");
	t.bsmod.mod_marker = 0xe;
	WARN(t.bsmod.is_d0_convict,  "EC Ditry Bits Marker is not aligned, d0 convict not marked correctly");
	WARN(!t.bsmod.is_d1_convict, "EC Ditry Bits Marker is not aligned, d1 convict not marked correctly");
	t.bsmod.mod_marker = 0xf;
	WARN(!t.bsmod.is_d0_convict, "EC Ditry Bits Marker is not aligned, d0 convict not marked correctly");
	WARN(!t.bsmod.is_d1_convict, "EC Ditry Bits Marker is not aligned, d1 convict not marked correctly");
	BUILD_BUG_ON(sizeof(union nvmeib_blkset_problem_report) != 2);	// u16
}

static void __verify_u32_lock_in_blockset_bits(void)
{
	const u64 binfo = (u64)nvmeib_stale_special_raid1.all;
	const u64 lock =  (u64)nvmeib_stale_special_raid1.lock_id.all;
	WARN((binfo != lock), "Wrong binfo structure. binfo=0x%llx, lock=0x%llx", binfo, lock);
	BUILD_BUG_ON(NVMEIB_BLKSET_INFO_TXID_MASK   != 0x000fffff);
	BUILD_BUG_ON(NVMEIB_BLKSET_INFO_DIRTY_MASK  != 0xfff00000);
}

static void __verify_u64_ec_dmd_bits(void)
{
	union nvmeibc_block_dp_ec_data_block_md md = {.raw = 0};
	// Verify that we can compare with u64 values
	BUILD_BUG_ON(BITS_PER_LONG != 64);
	BUILD_BUG_ON(sizeof(union nvmeibc_block_dp_ec_data_block_md) != sizeof(u64));
	BUILD_BUG_ON((NVMEIBC_DP_EC_DMD_BITS_JCI + NVMEIB_EC_JMDC_BITS_TX_ID) > 32);	// Cannot fit {TxID,JRI} in u32
	BUILD_BUG_ON((NVMEIBC_DP_EC_DMD_BITS_VERSION + NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS + NVMEIBC_DP_EC_PMD_BITS_EDIC + NVMEIB_EC_JMDC_BITS_TX_ID + NVMEIBC_DP_EC_DMD_BITS_JCI + 2*DB_DESCRIPTOR_BITS + RESERVED_MD_BITS) != BITS_PER_LONG);
	BUILD_BUG_ON((NVMEIBC_DP_EC_DMD_BITS_VERSION + NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS + NVMEIBC_DP_EC_DMD_BITS_EDIC + NVMEIB_EC_JMDC_BITS_TX_ID + NVMEIBC_DP_EC_DMD_BITS_JCI                        + RESERVED_MD_BITS) != BITS_PER_LONG);
	BUILD_BUG_ON(ilog2(NVMEIB_EC_MAX_JOURNAL_RANGES) > NVMEIBC_DP_EC_DMD_BITS_JCI);
	BUILD_BUG_ON(ilog2(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) > NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS);

	// Verify 'version' field of D and P is in the same bits
	md.D.version ^= ~0;
	md.P.version ^= ~0;
	WARN(md.raw != 0, "EC dmd wrong version bits = 0x%llx", md.raw);

	// Verify 'd2j_rng' field of D and P is in the same bits
	#ifdef DEBUG_SAVE_JENTRY
		md.D.d2j_rng ^= ~0;
		md.P.d2j_rng ^= ~0;
		WARN(md.raw != 0, "EC dmd wrong d2j bits = 0x%llx", md.raw);
	#endif
}

static void __verify_u64_ec_jmd_bits(void)
{
	BUILD_BUG_ON((NVMEIB_EC_JMDC_BITS_TX_ID + NVMEIB_EC_JMDC_BITS_TX_BMP + NVMEIB_EC_JMDC_BITS_VER) != 32); // cannot fit {TxID,TxBM,ver} in u32
	BUILD_BUG_ON((NVMEIB_EC_JMDC_BITS_TX_ID + NVMEIB_EC_JMDC_BITS_TX_BMP_ZIP + NVMEIB_EC_JMDC_BITS_VER + 2*NVMEIB_EC_JMDC_BITS_LINK) != 32); // Error wrong compilation params, cannot fit jmd into 32 bits
	BUG_ON(__stringify(NVMEIB_EC_JMDC_BITS_TX_BMP)[0] == '(');				// It should be macro of number without brases for printfs to work
	BUG_ON(__stringify(NVMEIB_EC_JMDC_BITS_TX_BMP_ZIP)[0] == '(');			// It should be macro of number without brases for printfs to work
}

#include "block/nvmeibc_block_globals.h" // Todo: Move this function out from this file. It does not belong here

typedef int (*t_update_params_fn)(struct nvmeibc_cinst_params_blk *p, const char *ioctl);

struct t_param_update_ioctl {
	const char *prefix;
	int prefix_len;
	t_update_params_fn fn;
};

static int __update_jentry_num_blocks(struct nvmeibc_cinst_params_blk *p, const char *ioctl)
{
	long val = 0;
	int rv = kstrtol(ioctl, 10, &val);
	p->binje = (u32)val;
	return rv;
}

static struct t_param_update_ioctl _param_update_tbl[] = {
	{"jstride=" , 8 /*strlen("jstride=")*/, __update_jentry_num_blocks}
};

int t_blok_clnt_globals_params_update(struct nvmeibc_cinst_params_blk *p, const char *ioctl)
{
	int i, rv, n_funcs = ARRAY_SIZE(_param_update_tbl);
	for (i = 0; i < n_funcs; i++) {
		const struct t_param_update_ioctl *upd = &_param_update_tbl[i];
		if (strncmp(upd->prefix, ioctl, upd->prefix_len) == 0) {
			rv = upd->fn(p, &ioctl[upd->prefix_len]);
			goto _out;
		}
	}
	rv = -EINVAL;			// Unknown parameter update
_out:
	return rv;
}

void nvmeibc_block_dp_fill_cinst_params_from_module_params(struct nvmeibc_cinst_params_blk *p)
{
	// Verify correctness of binje
	#define is_pow_2(x) (((x) & ((x) - 1)) == 0)
	if ((nvmeibc_jentry_num_blocks > NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY) ||
		(nvmeibc_jentry_num_blocks == 0) ||
		!is_pow_2(nvmeibc_jentry_num_blocks)) {
		_NE(t_01_binjecheck, DMESG_PREFIX() "wrong binje module param=@BINJE, resetting to 1", nvmeibc_jentry_num_blocks);
		nvmeibc_jentry_num_blocks = 1;
	}

	// Verify binje is matches jmd encoding
	if (nvmeibc_jentry_num_blocks != 1) {
		extern uint nvmeibc_jmd_wr_version;
		if (nvmeibc_jmd_wr_version == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED) {
			_NE(t_02_binjecheck, DMESG_PREFIX() "nvmeibc bug! version unpacked with binje=@BINJE. Setting to packed!", nvmeibc_jentry_num_blocks);
			nvmeibc_jmd_wr_version = NVMEIBC_JOURNAL_MD_VERSION_PACKED;
		}
	}
	p->binje = nvmeibc_jentry_num_blocks;
}

/****************************** Generic Commands ******************************/
#define __data_cmd_to_piggyback_addr(cmd) \
	__to4K((cmd)->reqs1.disk_address & LOCKSET_MASK)

static inline u64 __cmd_to_piggyback_addr(struct nvmeibc_block_command *c)
{
	const struct nvmeibc_block_command *c_data = dp_cmd_jour_to_data(c);
	return __data_cmd_to_piggyback_addr(c_data->iocmd);
}

/* Note: Dirty bit does not have 'rv' and we don't care if it was a success or
   failure. So we don't call the full completion of command callback:
   dev->dp.cmd_comp_cb(), but give a second, simpler completion on the cmd that
   casued by dirty bit turn-off */
static int __post_cmd_dirtybit_turnoff_cb(struct nvmeibc_d_rdma_comp *dc, struct nvmeibc_d_rdma_comp_tag tag) {
	struct nvmeibc_block_command *c = dp_cmds_get_cmd_from_comp(get_d_comp_of_pg(dc));
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	(void)tag;
	icore_ops->cb_called_comp(icore_ops, c->ds->disk, dc);
	dp_cmds_complete_cmd(c->cmdarr, c->my_leader, c);
	return 0;
}

void dp_cmds_piggyback_dbR1_on_write(struct nvmeibc_block_command *_cmd, const struct nvmeibc_dbits_tx *dbmap, u32 reserved)
{
	struct nvmeibc_disk_io_command *cmd  = _cmd->iocmd;
	struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(cmd);
	u64 *payload = &dp_cmds_get_piggyback_val(_cmd);
	cmd->lpb.handle = handle_of(_cmd->ds);
	cmd->lpb.addr = __data_cmd_to_piggyback_addr(cmd); // Never pigbacked on journal cmds
	dc->lock_cnsts = nvmeibc_raid1_get_lock_consts(nvmeibc_disk_segment_get_praid(_cmd->ds));
	((union nvmeib_blkset_info*)payload)->bits.dirty = dbmap->post.all_bits;
	((union nvmeib_blkset_info*)payload)->bits.txid  = reserved; 		// Daniel: Tmp debug code (coz those bits are not used)
	dc->opr = NVMEIBC_LOCK_BLKSET_INFO_WRITE;
	if (!nvmeibc_dbits_has_turn_off(dbmap)) {		// Piggyback dirtybit turn-on on cmd
		dp_cmds_add_generic_piggyback(cmd);
		dc->callback = NULL; 		// No dedicated callback
		dc->code = NVMEIBC_CMD_BLKSET_INFO_WR_PB;
	} else { 	// turn-off dbit will be executed at the end of the stage
		// EC-1473: Here for N-mirror: test for having turn-off and on simultanously. If so piggyback only turn on!
		cmd->comp.has_piggyback = false;			// By default, no dirtybit
		dc->callback = &__post_cmd_dirtybit_turnoff_cb;
		dc->code = NVMEIBC_CMD_BLKSET_INFO_WR_DR;
	}
	_ND(t_1pbw, ": Dbits=@DBITS cmd=@PTR has_pb=@BOOL_YN dc->opr=@OPR_TYPE dc->val={@BINFO,@LOCK_ENT_U64}",
	   dbmap->post.all_bits, _cmd, cmd->comp.has_piggyback, dc->opr, (u32)dc->lock.bi, dc->lock.id);
}

void dp_cmds_piggyback_info_on_write(struct nvmeibc_block_command *_cmd, union nvmeib_blkset_info v)
{
	struct nvmeibc_disk_io_command *cmd = _cmd->iocmd;
	struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(cmd);
	u64 *payload = &dp_cmds_get_piggyback_val(_cmd);
	cmd->lpb.handle = handle_of(_cmd->ds);
	cmd->lpb.addr =  __cmd_to_piggyback_addr(_cmd); // May be piggbacked on journal
	dc->lock_cnsts = nvmeibc_raid1_get_lock_consts(nvmeibc_disk_segment_get_praid(_cmd->ds));
	*((union nvmeib_blkset_info*)payload) = v;
	dc->opr = NVMEIBC_LOCK_BLKSET_INFO_WRITE;
	dp_cmds_add_generic_piggyback(cmd);
	dc->callback = NULL; // No dedicated callback
	dc->code = NVMEIBC_CMD_BLKSET_INFO_WR_PB;
	_ND(t_2pbw, ": Txid=@TXID Dbits=@DBITS cmd=@PTR has_pb=@BOOL_YN", v.bits.txid, v.bits.dirty, _cmd, cmd->comp.has_piggyback);
}

void dp_cmds_add_readlock_to_rldr(struct nvmeibc_block_command *rldr)
{
	struct nvmeibc_disk_io_command *cmd = rldr->iocmd;
	struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(cmd);
	const int lsi = nvmeibc_cllink_find_lock_by_cmd(rldr);	// Owner lock
	struct nvmeibc_cmd_lock *l = &rldr->cmdarr->locksets[lsi];
	const struct nvmeibc_disk_segment *seg_l = l->ds;
	const struct nvmeibc_raid1* r1 = nvmeibc_disk_segment_get_praid(seg_l);

	WARN(!dp_cmd_is_raid_leader(rldr), "nvmeibc bug ci=%d\n", (int)(rldr-rldr->cmdarr));
	cmd->lpb.handle = handle_of(seg_l);
	cmd->lpb.addr = l->address;
	dc->opr = NVMEIBC_LOCK_READ;

	cmd->comp.pigbck_lock = l;
	l->last_retry_report_time = l->first_try_time = jiffies;		// Todo: Move to separate func(). Much like in __request_lock()

	dc->lock_cnsts = nvmeibc_raid1_get_lock_consts(r1);
	dc->compare  = dc->lock_cnsts->unlocked_val;
	dc->exchange = r1->lid.all;
	dc->callback = &dp_locks_view_lock_sm;
	dc->cpu_mask_info = *cmd->reqs[0].cpu_mask_info; // STRUCT ASSIGNMENT
	cmd->comp.has_piggyback = false;			// By default, no dirtybit
	if (rldr->ds == seg_l) {
		const bool is_multi_read = (rldr->nraid_siblings > 1);
		if (!is_multi_read) {
			dp_cmds_add_generic_piggyback(cmd);
		} // Else: only after last cmd finishes views the lock, using rldr
	}	// Else can't piggyback even if we wanted to, retry reading the lock
	nflog(trace_1_add_readlock_to_rldr, "o=@OPERATION cmd=@CMD_PTR has_pb=@BOOL_YN", rldr->o, cmd, cmd->comp.has_piggyback);
	dc->code = l->type = (dp_cmds_pigbck_has_any(cmd) ? NVMEIBC_CMD_LOCK_READ_PB : NVMEIBC_CMD_LOCK_READ_DR);
	/* Anyways, Read lock must be viewed/verified once after all sibling cmds finish, much like dbits */
	rldr->raid_last_stage = max((int)rldr->raid_last_stage, (int)E_CMDS_STAGE_POST_IO_RDMA);
	rldr->use_io_apend_stages = true;
}

static bool __nvmeibc_cmd_data_and_metadata_pet_should_describe(struct nvmeibc_block_command const *bcmd, bool is_completion)
{
	const struct nvmeibc_raid1* r = nvmeibc_disk_segment_get_praid(bcmd->ds);
	struct nvmeibc_disk_io_command const* disk_io_cmd =  bcmd->iocmd;
	if (nvmeib_pet_journal_is_verbose(&bcmd->o->journal) == false){
		return false;
	}
	if (disk_io_cmd->reqs1.op != NVMEIB_BLOCK_IO_OP_READ && disk_io_cmd->reqs1.op != NVMEIB_BLOCK_IO_OP_WRITE){
		return false;
	}
	if (disk_io_cmd->reqs1.op == NVMEIB_BLOCK_IO_OP_READ && is_completion == false){ //sending read request
		return false;
	}
	if (disk_io_cmd->reqs1.op == NVMEIB_BLOCK_IO_OP_WRITE && is_completion == true){ //receiving write response
		return false;
	}
	if (!nvmeibc_raid_is_ec(r) && !nvmeibc_raid_is_mirror(r)) {
		return false;
	}
	return true;
}

static void __nvmeibc_cmd_data_and_metadata_pet_describe(struct nvmeibc_block_command const *bcmd, bool is_completion)
{
	struct nvmeibc_disk_io_command *disk_io_cmd =  bcmd->iocmd;
	struct nvmeib_data_buffer *ndb = disk_io_cmd->reqs1.ndb;
	struct nvmeib_pet_journal* journal = &bcmd->o->journal;
	bool const enable_edic_check = bcmd->o->nd->dp.enable_edic_check;

	u64 *data_first_content;
	struct nvmeib_scatterlist_block_iter it;
	union nvmeibc_block_dp_ec_data_block_md *md, *md_start;
	const u32 md_size = nvmeibc_sgmnt_sw_md_size(bcmd->ds);
	const struct nvmeibc_raid1* r = nvmeibc_disk_segment_get_praid(bcmd->ds);

	bool const should_trace = __nvmeibc_cmd_data_and_metadata_pet_should_describe(bcmd, is_completion);
	if(should_trace == false){
		return;
	}

	BUG_ON(ndb->length & (NVMEIBC_SECTOR_SIZE - 1)); // ndb->length is not a multiple of sector size
	md_start = (union nvmeibc_block_dp_ec_data_block_md *)disk_io_cmd->reqs1.md;
	nvmeib_scatterlist_block_iter_init(&it, NVMEIBC_SECTOR_SIZE, ndb->table.sgl, ndb->table.nents);
	while (nvmeib_scatterlist_block_iter_next(&it)) {
		md = (union nvmeibc_block_dp_ec_data_block_md *)((u8*)md_start + it.idx * md_size);
		data_first_content = (u64*)it.data;
		BUG_ON(data_first_content == NULL);
		/**
		* JBOD does not use metadata. Mirroring only cares about EDIC, as
		* it may be transaction id (used to mark what type of client wrote the
		* data). EC cares about everything.
		*/
		if (nvmeibc_raid_is_ec(r)) {
			BUG_ON(md_start == NULL); //we print on write.request & read.response, so md should be allocated
			NVMEIBC_IO_PET_MSG_NORM(
				journal,
				"    content(idx=%hhu, first_8b = 0x%llx, is_parity = %hhu, md = 0x%llx<union nvmeibc_block_dp_ec_data_block_md>)",
				numeric_downcast(u8, it.idx), *data_first_content, (u8)bcmd->is_parity, md->raw);
		} else if (nvmeibc_raid_is_mirror(r)) {
			//mirror datapath is configurable and may use or not use edic, but since we are playing with the backdoor, it is better to print it anyway
			if (enable_edic_check) {
				BUG_ON(md_start ==
				       NULL); //we print on write.request & read.response, so md should be allocated
				NVMEIBC_IO_PET_MSG_NORM(journal,
							"    content(idx=%hhu, first_8b = 0x%llx, edic = 0x%x)",
							numeric_downcast(u8, it.idx), *data_first_content,
							nvmeibc_block_dp_ec_md_get_edic(md, bcmd->is_parity));
			} else {
				NVMEIBC_IO_PET_MSG_NORM(journal, "    content(idx=%hhu, first_8b = 0x%llx)",
							numeric_downcast(u8, it.idx), *data_first_content);
			}
		}
	}
}

static void __nvmeibc_cmd_piggyback_request_pet_describe(struct operation *o, struct nvmeibc_disk_io_command const *cmd)
{
	struct nvmeibc_d_rdma_comp const *rdma_comp;

	if (!dp_cmds_pigbck_has_any(cmd))
		return;

	rdma_comp = dp_cmds_get_pigbck_comp_dc(cmd);
	if (rdma_comp->code == NVMEIBC_CMD_LOCK_READ_PB) { // read piggyback
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
			"    read_pb.request(addr=0x%llx, opr=%hhu<enum nvmeibc_disk_locks_opr>, code=%hhu<enum nvmeibc_rdma_intent>)",
			cmd->lpb.addr, numeric_downcast(u8, rdma_comp->opr), numeric_downcast(u8, rdma_comp->code));
	} else if (rdma_comp->code == NVMEIBC_CMD_BLKSET_INFO_WR_PB) { // write piggyback
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
			"    write_pb.request(addr=0x%llx, opr=%hhu<enum nvmeibc_disk_locks_opr>, code=%hhu<enum nvmeibc_rdma_intent>, binfo=%u<union nvmeib_blkset_info>)",
			cmd->lpb.addr, numeric_downcast(u8, rdma_comp->opr), numeric_downcast(u8, rdma_comp->code), (u32)rdma_comp->lock.bi);
	}
}

static void __nvmeibc_cmd_execute_disk_io_request_pet_describe(struct nvmeibc_block_command const *cmds, int cmd_idx)
{
	struct nvmeibc_block_command const *bcmd = &cmds[cmd_idx];
	struct nvmeibc_disk_io_command const *cmd = bcmd->iocmd;
	struct nvmeib_data_buffer const *ndb = cmd->reqs1.ndb;
	u8 const sgmnt_idx = numeric_downcast(u8, nvmeibc_dp_get_sgmnt_idx_from_ds(bcmd->ds));
	enum nvmeib_block_io_op op = cmds->o->op;
	u32 nlbas;

	if (op == NVMEIB_BLOCK_IO_OP_DISCARD) {
		nlbas = nvmeib_get_ndb_discard_range(bcmd, NVMEIB_DSM_RANGE_ENCODING_NATIVE).nlb;
	} else {
		nlbas = NVMEIBC_BYTE2SECTOR(ndb->length);
	}

	NVMEIBC_IO_PET_MSG_NORM(
		&cmds->o->journal,
		"disk_io.request(sgmnt=%hhu, dlba=0x%llx, nlbas=%u, raid_cur_stage=%hhu<enum e_cmds_stage>)",
		sgmnt_idx, __cmd_start(*bcmd), nlbas, (u8)cmds->raid_cur_stage);
}

static void __nvmeibc_cmd_execute_pet_describe(struct nvmeibc_block_command const *cmds, int cmd_idx)
{
	struct nvmeibc_block_command const *bcmd = &cmds[cmd_idx];
	struct nvmeibc_disk_io_command const *cmd = bcmd->iocmd;

	__nvmeibc_cmd_execute_disk_io_request_pet_describe(cmds, cmd_idx);
	__nvmeibc_cmd_piggyback_request_pet_describe(cmds->o, cmd);
	__nvmeibc_cmd_data_and_metadata_pet_describe(bcmd, /*is_completion=*/false);
}

int dp_cmds_execute_cmd(struct nvmeibc_block_command *cmds, int cmd_idx)
{
	struct nvmeibc_block_command   *bcmd = &cmds[cmd_idx];
	struct nvmeibc_disk_io_command *cmd =  bcmd->iocmd;
	enum nvmeib_block_io_op op = cmds->o->op;
	int rv = -EDOM;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	struct nvmeibc_profiler *profile = NULL;
	bool can_do_profiling = cmd->reqs1.op <= NVMEIB_BLOCK_IO_OP_DISCARD && io_op_is_rwt(op);
	dp_dbgdi_do_add_info(&cmds[cmd_idx], true);
	nflog(t_01_gp_exec_cmd, "n=@CMD_IDX, req_id=@REQ_ID_LLONG, @DLBA, cmds=@CMDS/@BCMD/@COMP, o=@OPERATION", cmd_idx, cmd->req_id, cmd->reqs1.disk_address, cmds, bcmd, &cmd->comp, cmds->o);
	BUG_ON((cmd->comp.comp_code != 0 && cmd->comp.comp_code != (int)HTR_INVALID_COMP_CODE) || bcmd->o_rv != 0); /* Command was executed twice? */

	// Start disk stats
	if (can_do_profiling) {
		profile = nvmeibc_get_raid_good_path_profile_for_rwt_op(bcmd->ds, op);
		nvmeibc_profiling_start_take_cmd_stats_for_op(nvmeibc_get_raid_good_path_profile_for_rwt_op(bcmd->ds, op), bcmd->ds->disk_operation_profiler, cmds->o, nvmeibc_profiling_get_cmd_stage(cmd), bcmd);
	}
	_ND(trace_dp_io_generic_cmds_dp_cmds_execute_cmd, "Going to execute: operation_code=@BLOCK_IO_OP cmds[@COMMAND_IDX].nlbas=@NLBAS", op, cmd_idx, bcmd->nlbas);
	__nvmeibc_cmd_execute_pet_describe(cmds, cmd_idx);
	if (unlikely(dp_cmds_does_require_jam(bcmd)))
		rv = icore_ops->execute_io_jour_blocks(icore_ops, bcmd->ds->disk, cmd);
	else
		rv = icore_ops->execute_io_blocks(icore_ops, bcmd->ds->disk, cmd);
	/* Be careful: Here cmds/op/locks might already be kfree() */
	if (rv < 0) {
		nflog(t_02_gp_exec_cmd, "Got an error @RV! req_id=@REQ_ID_LLONG, o=@OPERATION", rv, cmd->req_id, cmds->o);
		rv = -EIO;
		if (bcmd->ds && can_do_profiling) { // Immediately end profiler when no callback is expected
			nvmeibc_profiling_end_take_cmd_stats_for_op(profile, bcmd->ds->disk_operation_profiler, cmds->o, nvmeibc_profiling_get_cmd_stage(cmd), bcmd, rv);
		}
	} else {
		rv = 0; /* nvmeibc_disk_execute_io returns 1 for pending, ignore... */
	}
	_ND(trace_1_dp_io_generic_cmds_dp_cmds_execute_cmd, "rv=@RV", rv);
	return rv;
}

struct nvmeibc_block_command *dp_cmds_placement_new(u32 ncmds, void* locks_suffix, bool has_sgls)
{
	struct nvmeibc_block_command   *bcmds = locks_suffix;
	struct nvmeibc_disk_io_command *dcmds = ((void*)(&bcmds[ncmds]));	// Array of bcmds followed by dcmds
	struct nvmeib_data_buffer      *ndbs =  ((void*)(&dcmds[ncmds]));	// Array of dcmds followed by ndbs
	struct scatterlist             *sgls =  ((void*)(&ndbs[ ncmds]));	// Array of ndbs  followed by sgls
	u32 i;
	bcmds->used_placement_alloc = true;
	for (i = 0; i < ncmds; i++ ) {
		bcmds[i].iocmd = &dcmds[i];
		dcmds[i].disk_cmd.owner = &bcmds[i];
		dcmds[i].reqs1.ndb = &ndbs[i];
	}
	if (has_sgls) {
		for (i = 0; i < ncmds; i++ ) {
			bcmds[i].used_placement_sgl = true;
			ndbs[i].table.sgl = &sgls[i /* * nlbas */];
		}
	}
	return bcmds;
}

size_t dp_cmds_calc_size(u32 ncmds)
{
	BUILD_BUG_ON((((sizeof(struct nvmeibc_block_command  )/8)*8)) != sizeof(struct nvmeibc_block_command  ));		// Verify each bcmd is of multiple of 64 bits
	BUILD_BUG_ON((((sizeof(struct nvmeibc_disk_io_command)/8)*8)) != sizeof(struct nvmeibc_disk_io_command));
	BUILD_BUG_ON((((sizeof(struct nvmeib_data_buffer     )/8)*8)) != sizeof(struct nvmeib_data_buffer     ));
	return (ncmds * (sizeof(struct nvmeibc_block_command) + sizeof(struct nvmeibc_disk_io_command) + sizeof(struct nvmeib_data_buffer)));
}

struct nvmeibc_block_command *dp_cmds_kvzalloc(u32 ncmds)
{
	size_t alloc_size = dp_cmds_calc_size(ncmds);
	void *buf = my_kvzalloc(alloc_size, nvmeibc_dp_get_allow_io_gfp_flags());

	nvmesh_memmgr_metric_on_alloc_update(dp_commands, alloc_size, buf);

	if (buf) {
		struct nvmeibc_block_command *bcmds = dp_cmds_placement_new(ncmds, buf, false);
		bcmds->used_placement_alloc = false;
		bcmds->mem_allocated_size = alloc_size;
		return bcmds;
	}
	return NULL;
}

struct nvmeib_data_buffer *nvmeib_get_ndb(struct nvmeibc_block_command *cmd, int nentries, gfp_t gfp)
{
	struct nvmeib_data_buffer *ndb = cmd->iocmd->reqs1.ndb;
	ndb->ctrl_flags = 0;
	if (unlikely(!nentries)) {
		memset(&ndb->table, 0, sizeof(ndb->table));	// ready for sg_free_table(), Todo: Remove, cmd was already kzalloced
	} else if (cmd->used_placement_sgl) {
		ndb->table.nents = ndb->table.orig_nents = nentries;	// sgl already allocated
		//sg_mark_end(&ndb->table.sgl[nentries - 1]);
		ndb->inline_sgl = 1;							// Just for debug
	} else if (sg_alloc_table(&ndb->table, nentries, gfp))
		return NULL;
	nvmeib_data_reuse_buf_zero(&ndb->rcookie);   // Todo: Remove, cmd was already kzalloced
	return ndb;
}

void __nvmeib_put_ndb(struct nvmeib_data_buffer *ndb)
{
	if (ndb) {
		#if defined(BLKDEV_SIMULATOR)
			const struct nvmeib_data_reuse_buf_params *rcookie = &ndb->rcookie;
			WARN(((rcookie->channel_ver|rcookie->action) != 0ULL), "nvmeibc bug! mem leak - channel_ver=%llu, action=%u\n", rcookie->channel_ver, rcookie->action);
		#endif // Real transport layer has bugs which are solved via disk pause
		if (!ndb->inline_sgl)
			sg_free_table(&ndb->table);
		//my_kfree(ndb);								// Always allocated with its io_cmd, so never need to free this explicitly
	}
}

static void __nvmeibc_disk_io_command_free(struct nvmeibc_disk_io_command *cmd, bool is_not_ndb_owner, bool do_not_release_md)
{
	if (cmd->reqs) {
		if (!is_not_ndb_owner) {
			__nvmeib_put_ndb(cmd->reqs1.ndb);
			kfree(cmd->reqs1.trim);					// Typically NULL. used only in discard operations
		}
		if (likely(!do_not_release_md)) {		// Sync read/write stages share MD and must be realease only once
			nvmeibc_free_md(cmd->reqs1.md);
		}
	}
}

void dp_cmds_free_all(struct nvmeibc_block_command *cmds)
{
	struct nvmeibc_block_command *c;
	int i;
	if (unlikely(cmds == NULL))
		return;												// Upon allocation error
	for (i = 0, c = cmds; i < cmds->ncmds; i++, c++) {
		if (unlikely(c->my_stage == E_CMDS_STAGE_WRITE_JOURNAL)) { // TODO(DORON) - is the below addition a bug?
			BUG_ON(c->iocmd->reqs1.disk_address != 0);	/* Journal Blocks were not returned to JAM */
			BUG_ON(!c->is_not_ndb_owner);	/* Todo: Should always be true*/
		}
		if (c->gen_cmd){
			my_kvfree(c->gen_cmd);
			c->gen_cmd = NULL;
		}
		__nvmeibc_disk_io_command_free(c->iocmd, c->is_not_ndb_owner, c->do_not_realease_md);
	}

	if (!cmds->used_placement_alloc) {
		WARN_ON(!cmds->mem_allocated_size);
		nvmesh_memmgr_metric_on_free_update(dp_commands, cmds->mem_allocated_size);
		my_kvfree(cmds);
	}
}

static enum nvmeibc_disk_io_cmd_originator __nvmeib_block_io_op_to_cmd_originator(enum nvmeib_block_io_op op)
{
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_READ:		return NVMEIBC_DISK_IO_CMD_ORIG_IO_READ;
	case NVMEIB_BLOCK_IO_OP_WRITE:		return NVMEIBC_DISK_IO_CMD_ORIG_IO_WRITE;
	case NVMEIB_BLOCK_IO_OP_DISCARD:	return NVMEIBC_DISK_IO_CMD_ORIG_IO_DISCARD;
	default: break;
	}

	if (op & (NVMEIB_BLOCK_IO_OP_RECOVER_STALE | NVMEIB_BLOCK_IO_OP_REC_SPARE | NVMEIB_BLOCK_IO_OP_MAINTAIN_RESOLVE_ALL_BINFO)) {
		return NVMEIBC_DISK_IO_CMD_ORIG_RECOV;
	}

	return NVMEIBC_DISK_IO_CMD_ORIG_NONE;
}

void dp_cmds_req_fill(struct operation* o, int n, struct nvmeibc_disk_segment *ds)
{
	struct nvmeibc_block_command *cmds = o->cmds;
	cmds[n].iocmd->req_id = dp_cmds_req_alloc_unique_id();
	cmds[n].iocmd->orig = __nvmeib_block_io_op_to_cmd_originator(o->op);
	cmds[n].ds = ds;
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	cmds[n].iocmd->v_disk_stats = ds->v_disk_stats;
#endif
	cmds[n].iocmd->reqs = &cmds[n].iocmd->reqs1;
	cmds[n].iocmd->reqs->cpu_mask_info = &o->cpu_mask_info;
	cmds[n].cmdarr = cmds;
	cmds[n].o_rv = 0;
	cmds[n].iocmd->comp.cmd = cmds + n;
	cmds[n].o = o;
	#ifdef DEBUG_UNCOMPLETED
		cmds[n].iocmd->reqs[0].nlbas  = cmds[n].nlbas; // Daniel: For Omri-Man
	#endif
	nflog(flog_dp_io_generic_cmds_dp_cmds_req_fill, "Filled request_id=@REQ_ID_LLONG for commands=@COMMANDS[@COMMAND_IDX], disk=@DISK, o=@OPERATION", cmds[n].iocmd->req_id, cmds, n, ds->disk, o);
}

static void __wq_autofail_lkd_bio_cmd(struct work_struct *w)
{
	struct nvmeibc_disk_io_command *io_cmd = container_of(w, struct nvmeibc_disk_io_command, disk_cmd.auto_fail_work);
	struct nvmeibc_block_command *cmd = io_cmd->comp.cmd;
	struct nvmeib_data_reuse_buf_params *rcookie = get_rcookie_ptr(io_cmd);
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	if (rcookie->action) { /* If cookie exists, must free it */
		if (rcookie->channel_ver)
			icore_ops->reused_bb_release(icore_ops, cmd->ds->disk, rcookie);
		nvmeib_data_reuse_buf_zero(rcookie);	// Save failed, dont ask anything
	}
	cmd->o->nd->dp.cmd_comp_cb(&io_cmd->comp, nvmeibc_d_iocmd_comp_tag_make());
}

static void __return_completion_on_unset_cmds(const struct operation *o, struct nvmeibc_block_command *c)
{
	struct nvmeibc_disk_io_command *iocmd = c->iocmd;
	nflog(t_02_cmdsexec, "request_id=@REQ_ID_LLONG operation=@OPERATION", iocmd->req_id, o);
	if (nvmeib_block_io_op_is_bio_op(o->op)) {		// Multi-stage, use work-queue to avoid stack overflow of many completions
		INIT_WORK(&iocmd->disk_cmd.auto_fail_work, __wq_autofail_lkd_bio_cmd);	// Asyncrously autofail locked command
		BLKCMP_ANY_schedule_work(&iocmd->disk_cmd.auto_fail_work);
	} else {												// Sync operation: give direct callback
		o->nd->dp.cmd_comp_cb(&iocmd->comp, nvmeibc_d_iocmd_comp_tag_make()); 				// Same as calling __wq_autofail_lkd_bio_cmd(&iocmd->disk_cmd.auto_fail_work); directly
	}
}

int dp_cmds_tryexec_cmd(struct nvmeibc_block_command *cmds, int ci, const int error_on_no_execution)
{
	struct nvmeibc_disk_io_command *iocmd = cmds[ci].iocmd;
	const bool exec_cmd = (iocmd->comp.comp_code == 0); /* BIO -> Lock broken / prev-cmd failed,  Sync -> Do not send cmd */
	if (exec_cmd) {
		nflog(t_01_cmdsexec, "request_id=@REQ_ID_LLONG operation=@OPERATION", iocmd->req_id, cmds->o);
		if (dp_cmds_execute_cmd(cmds, ci) >= 0)
			return 1;	// Cmd successfully sent, Warning: Here cmds/op/locks might be free()
		iocmd->comp.comp_code = error_on_no_execution;
	} else {
		dp_dbgdi_do_add_info(&cmds[ci], false);					// Inject poison before read
	}
	__return_completion_on_unset_cmds(cmds->o, &cmds[ci]);		// Warning: Here cmds() might be free()
	return -1;
}

static void __free_detached_cmds_from_op(struct nvmeibc_block_command *cmds)
{
	__uncompleted_cmds_list_rmv(&cmds->iocmd);
	dp_cmds_free_all(cmds);
}

// In case QLC parital destage manipulated the ndb->table.sgl pointer (see __update_command_sgl) we need to restore the original value before freeing the ndb
static void __restore_orig_sgls_of_cmds_if_needed(struct nvmeibc_block_command *cmds, struct scatterlist *sgls[])
{
	int i;
	for (i = 0; i < cmds->ncmds; i++) {
		if (sgls[i])
			cmds[i].iocmd->reqs1.ndb->table.sgl = sgls[i];
	}
}

static void __finish_cmds_comp_oper(struct operation *o)
{
	struct nvmeibc_datapath *dp = &o->nd->dp;
	struct nvmeibc_cmd_lock *locks = o->locks;	// Cahce pointers on stack because 'o' is freed
	struct nvmeibc_block_command *cmds = o->cmds;
	struct scatterlist **orig_sgls = (typeof(orig_sgls))o->md_op.orig_sgls;		// This IO never uses this. Todo, make a better check of direct usage
	bool retry_required = false;
	bool embeded_commands_without_locks = o->flags.is_clmat_embedded;	// Cache value, o will be valid, make sure to resubmit or free not both
	int o_rv = 0;
	dp->calc_comp_state(cmds, &o_rv, &retry_required);
	nflog(t_dpio01, "@FUNCTION: o=@OPERATION, cmds=@CMDS", __FUNCTION__, o, cmds);
	nvmeibc_profiling_end_take_stats(nvmeibc_get_raid_good_path_profile_for_rwt_op(cmds->ds, o->op), o, o_rv);
	nvmeibc_operation_complete(o, retry_required, o_rv);
	cmds->o = NULL; /* operation 'o' does not exist anymore */
	if (orig_sgls)
		__restore_orig_sgls_of_cmds_if_needed(cmds, orig_sgls);
	__free_detached_cmds_from_op(cmds);// Now free the cmds
	if (locks)
		dp->allow_locks_rel_debug(locks);
	else if (embeded_commands_without_locks) {	// JBOD/Raid-0 embeding without locks
		if (o->flags.resubmit_operation_no_free) {	// Operation should be resubmitted do not free it
			o->flags.resubmit_operation_no_free = false;
			nvmeibc_io_resubmitter_retry_op(o);
		} else {
			nvmeibc_operation_free_bio_part(o->bios[0]);
		}
	}
}

void nvmeibc_operation_compressed_op_pet_dump_bio(const struct operation *o)
{
	const struct nvmeibc_block_command *rldr = o->cmds;		// For now print info of first rldr only
	const u64 topo = (u64)o->topo->debug_unique_index;
	const struct nvmeibc_raid1* raid = nvmeibc_disk_segment_get_praid(rldr->ds);
	const union nvmeibc_raid1_io_pet_status topo_status = nvmeibc_raid1_io_pet_describe_state(raid);

	if (o->nd->dp.enable_care_about_txid && o->op == NVMEIB_BLOCK_IO_OP_WRITE) {
		//TODO PET: assign dbg_id when operation is created;
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
									"operation.execute(dbg_id=%u, topology=(index=%llu, status=%llu<union nvmeibc_raid1_io_pet_status>) binfo=0x%x<union nvmeib_blkset_info> dbg_cntrs=0x%llx<union operation_dbg_cntrs>)",
									o->dbg_id, topo, topo_status.all, rldr->rld.pre.all, o->dbg_cntrs.raw);

	} else {
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
									"operation.execute(dbg_id=%u, topology=(index=%llu, status=%llu<union nvmeibc_raid1_io_pet_status>) dbg_cntrs=0x%llx<union operation_dbg_cntrs>)",
									o->dbg_id, topo, topo_status.all, o->dbg_cntrs.raw);
	}
}


static void __goodpath_operation_compressed_op_dump_bio(const struct operation *o)
{
	const struct nvmeibc_block_command *rldr = o->cmds;		// For now print info of first rldr only
	const u64 start_lba = get_op_start_lba(o), nlbas = get_op_nlbas(o);
	const u64 topo = (u64)o->topo->debug_unique_index;
	const u32 vol_id = nvmeibc_volume_short_id(o->nd);
	// Attention!!! this good path compressed bitfield must stay fast and compact! Never add dynamic sized values, only small known sizes.
	if (o->op == NVMEIB_BLOCK_IO_OP_READ) {
		NVMEIB_LOG_GOODPATH("{@O_DBG_ID} @BIODUMP_BINFO_N", _T, goodpath_nvmeibc, op_dump_bio_r,   o->dbg_id, o->op, vol_id, topo, start_lba, nlbas);
	} else {		// Write/Trim
		if (o->nd->dp.enable_care_about_txid) {
			NVMEIB_LOG_GOODPATH("{@O_DBG_ID} @BIODUMP_BINFO_Y", _I, goodpath_nvmeibc, op_dump_bio_wty, o->dbg_id, o->op, vol_id, topo, start_lba, nlbas, rldr->rld.pre.all);
		} else {
			NVMEIB_LOG_GOODPATH("{@O_DBG_ID} @BIODUMP_BINFO_N", _I, goodpath_nvmeibc, op_dump_bio_wtn, o->dbg_id, o->op, vol_id, topo, start_lba, nlbas);
		}
	}
}

void nvmeibc_operation_compressed_op_dump_bio(const struct operation *o)
{
	if (!nvmeibc_operation_is_bio(o)) {
		return;
	}
	__goodpath_operation_compressed_op_dump_bio(o);
}

static void __nvmeibc_operation_comp(struct operation *o)
{
	nvmeibc_pages_free(&o->pages, get_tcp_mode_of_operation(o));
	__finish_cmds_comp_oper(o);	   // 'o' was freed: cmds->o == NULL
}

static void __nvmeibc_operation_wq_copy_and_comp(struct workqe_struct *work)
{
	struct operation *o = container_of(work, struct operation, work_throttled);
	vv_bio_inter_copy_private_read_blocks_to_bio_if_needed(o);
	__nvmeibc_operation_comp(o);
	//operation is dead here
}

void nvmeibc_operation_put(struct operation *o, int n_refs)
{
	const int remain_o = nvmeibc_atomic_sub_return(n_refs, &o->n_uncomp_raids);

	BUG_ON(n_refs <= 0);
	if (remain_o > 0)
		return;			// This raid is done, 'o' waits for other raids
	WARN((remain_o < 0), "Suspected bug in nvmeibc rem=%d!\n", remain_o);
	if (nvmeibc_operation_is_bio_copy_needed_for_read(o) && get_tcp_mode_of_operation(o)){
		WQ_INIT_WORK(&o->work_copy_to_bio, __nvmeibc_operation_wq_copy_and_comp);
		dp_block_schedule_work(o->cpu_id, &o->work_copy_to_bio);
	} else {
		vv_bio_inter_copy_private_read_blocks_to_bio_if_needed(o);
		__nvmeibc_operation_comp(o);
	}
	//operation is dead here
}

void nvmeibc_operation_get(struct operation *o, int n_refs)
{
	nvmeibc_atomic_add(n_refs, &o->n_uncomp_raids);
}

static void __release_locks_of_completed_command(struct nvmeibc_block_command *cmds, int ci);
static void __try_overcome_read_fail(struct nvmeibc_d_iocmd_comp *comp);
#define can_read_fail_retry(ec) ((!is_transient_disk_error(ec)))

static inline void __replace_readfail_rv_with_try_again(
	struct nvmeibc_block_command *rldr, const enum e_cmds_stage stg, int i)
{
	for (;i<rldr->nraid_siblings && (rldr[i].my_stage == stg); i++) {
		if (can_read_fail_retry(rldr[i].o_rv)) {
			rldr[i].o_rv = -EAGAIN;
		}
	}
}

#ifdef DEBUG_TRANSFERS
static void DEBUG_TRANSFERS_verify_core_stuff(const struct nvmeibc_block_command *c, int i)
{
	const struct nvmeibc_disk_io_command *iocmd = c ? c->iocmd : NULL;
	if (c && iocmd) {
		if (iocmd->disk_cmd.in_flight) {
			_NE_dmesg(t_0l_dp_dbg_tools, "cmd @BLOCK_COMMAND (idx @CMD_IDX) iocmd @BLOCK_COMMAND - still in flight!", c, i, iocmd);
			BUG();
		}
#	ifdef DEBUG_TRANSFERS_CHECK_NDB_MAPPED
		if (iocmd->disk_cmd.cmd_type == NVMEIBC_DISK_CMD_IO && iocmd->reqs1.ndb_mapped) {
			_NE_dmesg(t_0m_dp_dbg_tools, "cmd @BLOCK_COMMAND (idx @CMD_IDX) iocmd @BLOCK_COMMAND - still mapped!", c, i, iocmd);
			BUG();
		}
#	endif
	}
}
#else
	#define DEBUG_TRANSFERS_verify_core_stuff(c,i)  ({(void)c; (void)i;})
#endif

void dp_cmds_free_split(struct nvmeibc_block_command *cmds)
{ // cmds were disconnected from the operation due to trim-split don't touch anything except freeing the commands
	__free_detached_cmds_from_op(cmds);
}


static void __nvmeibc_cmd_piggyback_response_pet_describe(struct operation *o, struct nvmeibc_block_command *rldr, struct nvmeibc_block_command *cmd)
{
	struct nvmeibc_d_rdma_comp *dc;

	if (!dp_cmds_pigbck_has_any(rldr->iocmd))
		return;

	dc = dp_cmds_get_pigbck_comp_dc(rldr->iocmd);
	// For both, return value is logged as part of __nvmeibc_cmd_data_and_metadata_pet_describe().
	if (dc->code == NVMEIBC_CMD_LOCK_READ_PB) { // read piggyback
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
			"    read_pb.response(contending=0x%x<union nvmeib_lock_id>, lock_bi=0x%x<union nvmeib_blkset_info>)",
			(u32)dc->lock.id, (u32)dc->lock.bi);
	} else if (dc->code == NVMEIBC_CMD_BLKSET_INFO_WR_PB) { // write piggyback
		NVMEIBC_IO_PET_MSG_NORM(&o->journal,
			"    write_pb.response(addr=0x%llx, opr=%hhu<enum nvmeibc_disk_locks_opr>, code=%hhu<enum nvmeibc_rdma_intent>)",
			cmd->iocmd->lpb.addr, numeric_downcast(u8, dc->opr), numeric_downcast(u8, dc->code));
	}
}

static void __nvmeibc_cmd_disk_io_complete_response_pet_describe(struct operation *o, struct nvmeibc_block_command *cmd)
{
	NVMEIBC_IO_PET_MSG(&o->journal,
		"disk_io.response(sgmnt=%hhu, o_rv=%d, comp_code=%d)",
		cmd->o_rv ? NVMEIB_PET_SEVERITY_WARNING : NVMEIB_PET_SEVERITY_NORMAL,
		numeric_downcast(u8, nvmeibc_dp_get_sgmnt_idx_from_ds(cmd->ds)), cmd->o_rv, cmd->iocmd->comp.comp_code);
}

static inline void __nvmeibc_cmd_completion_pet_describe(struct operation *o, struct nvmeibc_block_command *cmds, int li)
{
	struct nvmeibc_block_command *rldr = &cmds[li];
	int i = 0;

	for (i = li; i < li + cmds[li].nraid_siblings; ++i) {
		struct nvmeibc_block_command *cmd = &cmds[i];
		if (rldr->raid_cur_stage != cmd->my_stage || cmd->do_not_send)
			continue;
		__nvmeibc_cmd_disk_io_complete_response_pet_describe(o, cmd);
		__nvmeibc_cmd_piggyback_response_pet_describe(o, rldr, cmd);
		__nvmeibc_cmd_data_and_metadata_pet_describe(rldr, /*is_completion=*/true);
	}
}

void dp_cmds_complete_cmd(struct nvmeibc_block_command *cmds, int ci, struct nvmeibc_block_command *this_cmd /* DEBUG_TRANSFERS */)
{
	struct operation *o = cmds->o;
	int n_uncompleted_cmds;
	DEBUG_TRANSFERS_verify_core_stuff(this_cmd, ci);
	n_uncompleted_cmds = nvmeibc_atomic_dec_return(&cmds[ci].n_uncompleted_cmds);
	__uncompleted_cmds_list_comp(&cmds->iocmd, n_uncompleted_cmds);
	if (n_uncompleted_cmds > 0)
		return;					// Waiting for other commands
	if (unlikely(n_uncompleted_cmds < 0)) {
		WARN(true, "nvmeibc bug! cmds=%p cmds->req_id=0x%llx n_uncompleted_cmds=%d, op=%d\n", cmds, cmds->iocmd->req_id, n_uncompleted_cmds, o->op);
	}
	#ifdef DEBUG_TRANSFERS
		{	int i;
			for (i = ci; i < ci + cmds[ci].nraid_siblings; i++) {
				DEBUG_TRANSFERS_verify_core_stuff(&cmds[i], i);
			}
		}
	#endif
	__nvmeibc_cmd_completion_pet_describe(o, cmds, ci);
	BLKCMP_IO_ASYNC_RESUME_CMP(dp_cmds_done_stage_overcome_failure(cmds, ci));
}

void dp_cmds_done_stage_overcome_failure(struct nvmeibc_block_command *cmds, int ci)
{
	struct nvmeibc_block_command *rldr = &cmds[ci];
	__attribute__ ((unused)) struct operation *o = cmds->o;
	if (likely(!rldr->use_read_fail_fix_blockset)) {
		enum e_cmds_stage stg = rldr->raid_cur_stage; // QLC read failure can (also) occur after we check QLC MD/EDIC in stage E_CMDS_STAGE_POST_IO_RDMA, but the commands are of stage E_CMDS_STAGE_DO_IO_AND_PAR
		int err, i = dp_cmds_get_first_cmd_of_stage(rldr, stg);
		if ((i >=0)&&(rldr[i].iocmd->reqs1.op == NVMEIB_BLOCK_IO_OP_READ)) { // Only if the current stage was read-data or preread for write
			for (err = 0; (!err && (i < rldr->nraid_siblings) && (rldr[i].my_stage == stg)); i++)	// Find first error: We might have multiple failures, make sure that we go over them in order
				err = rldr[i].o_rv;
			if (err && can_read_fail_retry(err)) { // Handle readfail scenario here. if we can retry read fail continue
				i--;												// cmds[i] is now the first read failed cmd, maybe there are others
				rldr->use_read_fail_fix_blockset = true;
				nvmeibc_atomic_set(&rldr->n_uncompleted_cmds, 1);	// Additional call
				__replace_readfail_rv_with_try_again(rldr, stg, i);
				__try_overcome_read_fail(&rldr[i].iocmd->comp);
				return;												// Callback will arrive to this function
			}
		}
	} else { // READ fail recover completed. Dont care about result. if failed, EPERM_READ_FAIL_NO_RETRY is on at least one completion and will fail later
		rldr->use_read_fail_fix_blockset = false;
	}
	BLKCMP_IO_ASYNC_RESUME_CMP(dp_cmds_next_stage_execute(cmds, ci));
}

void dp_transition_to_unlocked_blockset_sm(struct nvmeibc_block_command *cmds, int ci, int ow_i)
{
	struct nvmeibc_cmd_lock *locksets = cmds->locksets;
	const struct nvmeibc_datapath *dp = &cmds->o->nd->dp;
	WARN_ON(locksets[ow_i].status == NCL_STATUS_DONE);			// May be broken or taken, but still reduire to process it
	dp->calc_should_abandon(cmds, ow_i);
	nflog(t_2rlocc, "releasing locks: cmds=@CMDS[@CI] lock=@LOCK[@LSI]",cmds, ci, locksets, ow_i);
	dp_locks_release_locks_sibs(locksets, ow_i);
}

static void __release_locks_of_completed_command(struct nvmeibc_block_command *cmds, int ci)
{
	struct nvmeibc_cmd_lock *locksets = cmds->locksets;
	struct operation *o	= cmds->o;
	int lsi, ncmds, n_refs;
	if (cmds[ci].nlocks_take_before_cmd == 0) {
		BLKCMP_IO_ASYNC_RESUME_CMP();
		return;									// Unportected command, do not transition to locks release state machine
	}
	WARN_ON(cmds[ci].o_rv == -EDEAD);
	n_refs = dp_cmds_get_next_raid_leader(&cmds[ci]);	// Same logic as when linking cmds<-->locks
	for_each_primary_owner(lsi, locksets) {
		struct nvmeibc_cmd_lock *lo = &locksets[lsi];
		if (!nvmeibc_clmat_is_linked(o->CLmat, ci, lsi, locksets))
			continue;

		ncmds = nvmeibc_atomic_sub_return(lo->n_siblings*n_refs /* num cmds, each protected by all siblings of owner lock*/, &lo->ncmds);
		if (unlikely(ncmds < 0)) {
			WARN(1, "bvmeibc bug! lsi=%d, ncmds=%d\n", lsi, ncmds);
			__dump_operation(o);
		} else if (ncmds) {
			_ND(t_1rlocc, "Not releasing now: lsi=@LSI, ncmds=@NCMDS", lsi, ncmds);
		} else {
			BLKCMP_IO_ASYNC_RESUME_CMP(dp_transition_to_unlocked_blockset_sm(cmds, ci, lsi));
		}
	}
}

#include "block/recovery/nvmeibc_block_dp_sync_api.h"
/* Try to overcome permanent disk read fail via reading redundant data from
   other segments (parity/mirroring) via sync operation. Ignore no-retry bit.
   IO cmds will fail after sync completes and on next retry, IO will succeed.*/
static int __try_overcome_read_fail_on_sync_done_cb(void* context, int err)
{	/* Callback typeof nvmeibc_sync_cb_t */
	struct nvmeibc_d_iocmd_comp *comp = context;
	struct nvmeibc_block_command *cmd = comp->cmd, *cmds = cmd->cmdarr;
	switch (err) {
		case EPERM_READ_FAIL_NO_RETRY:	/* sync failed in double read error. */
		case NVME_SC_DNR: 				/* DNR bit is on */
			comp->comp_code = err;		/* Sync failed, give Err to kernel!!!*/
			break;
		case 0:
			//comp->comp_code = 0;		// Todo: Write the exact conditions under which sync fills sg-buf of caller, So 'cmd' contains correct info
			//When line above is diabled-IO will get a retry and cmd will be read again, even though this is not needed
			break;
		default:
			/* Daniel: Todo, analyze read failure error more precisely */
			break;
	}

	cmd->o_rv = comp->comp_code;        /* Final result of command */
	dp_cmds_complete_cmd(cmds, cmd->my_leader, cmd);	// Now all IOs complete and next stage is pending read fail sync
	return 0;
}

static void __try_overcome_read_fail(struct nvmeibc_d_iocmd_comp *comp)
{
	struct nvmeibc_block_command *cmd = comp->cmd;
	struct nvmeibc_cmd_lock *locksets = cmd->cmdarr->locksets;
	int lsi, rv;

	if (!locksets) { // Attempt to overcome read fail in jbod is NO RETRY
		goto _failed_to_overcome;	/* Jbod - no redundant data to use */
	}
	lsi = nvmeibc_cllink_find_lock_by_cmd(cmd);
	comp->comp_code = -EAGAIN;	/* Change rv to retry after sync complets */
	rv = nvmeibc_sync_read_failure(&locksets[lsi], __try_overcome_read_fail_on_sync_done_cb, comp);
	if (rv == 0) 	/* Will return with cb() after sync finishes */
		return;

	/* Sync failed to start & WILL NEVER be able to fix, finilize this cmd inline */
	comp->comp_code = EPERM_READ_FAIL_NO_RETRY;	/* No retry */
_failed_to_overcome:
	// When volume cannot trigger readfail sync (too many unavailable or jbod) update toma!
	if ((comp->comp_code != 0) && (should_notify_toma(comp->comp_code) ||
		(comp->comp_code == EPERM_READ_FAIL_NO_RETRY))) {
		__send_toma_cmd_help(comp, U32_MAX, 0);
	}
	__try_overcome_read_fail_on_sync_done_cb(comp, comp->comp_code);
}

void dp_cmds_analyze_rv_and_complete(struct nvmeibc_d_iocmd_comp *comp)
{
	struct nvmeibc_block_command *cmd = dp_cmds_get_cmd_from_comp(comp), *cmds = cmd->cmdarr;
	const int ci = (cmd - cmds);
	on_disk_hook(dp_cmds_analyze_rv_and_complete, cmd->ds->disk, io_cmd_completion, cmd);

	if (comp->comp_code && is_transient_disk_error(comp->comp_code)) {
		OPERATION_DBG_CNTR_INC(cmd->o, n_dcmd_failed);
		IO_STATS_INCR(&cmd->o->nd->dp.io_stats, DP_IO_STATS_SOFTWARE_READ_FAIL_COUNT);
	} else if (is_non_retriable_error(comp->comp_code)) {
		IO_STATS_INCR(&cmd->o->nd->dp.io_stats, DP_IO_STATS_DNR_BAD_SECTORS);
	}

	if (likely(!comp->comp_code)) {
		dp_dbgdi_do_rdr_info(cmd);
	} else if (cmd->iocmd->reqs1.op == NVMEIB_BLOCK_IO_OP_READ) {
		if (is_transient_disk_error(comp->comp_code)) {
			NVMEIBC_ERROR_TAG(transient_disk_error);
			nvmesh_error_tag_update(transient_disk_error, comp->comp_code);
			cmd->o_rv = -EAGAIN; /* Induce retry */
		} else {
			NVMEIBC_ERROR_TAG(permanent_read_fail);
			nvmesh_error_tag_update(permanent_read_fail, comp->comp_code);
			cmd->o_rv = comp->comp_code; /* Perm-read-fail, propagate further */
		}
	} else {
		NVMEIBC_ERROR_TAG(modify_disk_fail);
		nvmesh_error_tag_update(modify_disk_fail, comp->comp_code);
		cmd->o_rv = comp->comp_code;
		if (should_notify_toma(comp->comp_code)) {
			__send_toma_cmd_help(comp, U32_MAX, 0);
		}
	}

	if (unlikely(!cmd->use_stages /*Trim*/)) {
		__release_locks_of_completed_command(cmds, ci);
	}

	_ND(t_1dcarac, "cmds=@CMDS comp=@COMP, cmds=@CMDS[@MY_LEADER]", cmd, comp, cmds, cmd->my_leader);
	 dp_cmds_complete_cmd(cmds, cmd->my_leader, cmd);
	/* Be careful: Here cmds/op/locks might already be kfree() */
}

void dp_cmds_execute_stageless_trim_cmds(struct nvmeibc_block_command *cmds, int ow_i, int locks_rv)
{
	struct operation *o = cmds->o;
	struct nvmeibc_cmd_lock *locksets = o->locks;
	const int n_lock_copies = locksets[ow_i].n_siblings;
	const int error_on_no_execution = locks_rv ? locks_rv : -ENXIO;	// Default error if cmd is not execed
	int ci;
	bool execute_cmd = (locks_rv == 0);	// Should execute the current command
	BUG_ON(o->op != NVMEIB_BLOCK_IO_OP_DISCARD);
	nvmeibc_operation_get(o, LARGE_DEBUG_VALUE);	// Avoid cmds/op kfree during loop
	for (ci = 0; ci < cmds->ncmds; ci++) {
		if (!nvmeibc_clmat_is_linked(o->CLmat, ci, ow_i, locksets))
			continue;			/* cmd is irrelevant to this lock */

		if (!execute_cmd)		/* A single broken lock or prev cmd prevents execution of the rest of cmds */
			cmds[ci].iocmd->comp.comp_code = error_on_no_execution;
		if (nvmeibc_atomic_sub_return(n_lock_copies, &cmds[ci].nlocks))
			continue; /* Trim cmd still pending at least one another lock*/
		if (dp_cmds_tryexec_cmd(cmds, ci, error_on_no_execution) < 0)
			execute_cmd = false; /* Also abort the rest of the commands */
	}
	nvmeibc_operation_put(o, LARGE_DEBUG_VALUE);
}

void dp_transition_to_locked_cmds_sm(struct nvmeibc_cmd_lock *locksets, int ow_i)
{
	struct nvmeibc_block_command *cmds = locksets->cmds;
	const int n_lock_copies = locksets[ow_i].n_siblings;
	int locks_rv = dp_locks_calc_blockset_rv(&locksets[ow_i]);
	struct operation *o = cmds->o;
	/* If lock could not be acquired, all cmds which need it are not executed */
	if (cmds->use_stages) {	// Commands state machine which uses stages is within 1 blockset
		struct nvmeibc_cmd_lock *lo = &locksets[ow_i];
		const union nvmeib_blkset_info binfo = dp_locks_get_TxID_dbits(locksets, ow_i, false);
		struct nvmeibc_block_command *rldr = nvmeibc_cllink_find_cmd_by_lock(locksets, ow_i);
		BUG_ON(nvmeibc_atomic_read(&rldr->nlocks) != n_lock_copies);
		nvmeibc_atomic_set(&rldr->nlocks, 0); // Daniel: atomic_sub is slower than set(0)
		rldr->rld.pre.all = binfo.all;
		rldr->rld.post.all = rldr->rld.pre.all;			// 'Post' will likely to be overwritten. However, This line is crucial, if rldr state machine is not executed, post must be initialized, otherwise upon locks transfer, our 'post' may become the 'pre' of the next operation.
		if (!verify_binfo_is_legal(lo->ds, rldr->rld.pre, lo->address, 'r')) {
			nvmeibc_block_suspend(o->nd, NULL, NULL);		/* Critical error, any further action will cause data corruption - suspend. This will make the topology phased out, and the operation will terminate. No need to explicitly set error in this flow. */
		}
		if (o->nd->dp.exec_func_on_locks_tkn)
			locks_rv = o->nd->dp.exec_func_on_locks_tkn(rldr, locks_rv);
		if (locks_rv > 0) {
			/* R6 writes / Carrier-on-locks-taken: Async callback will continue execution or auto-failure */
		} else {
			const int ci = (int)(rldr - cmds);
			dp_cmds_execute_first_stage(cmds, ci, locks_rv); // R1,R0,R6-reads
		}
		return; // Async execution continues via stages
	} else {	// Trim - Execution of all cmds, without stages/state mahcine
		BLKCMP_IO_ASYNC_AWAIT(dp_cmds_execute_stageless_trim_cmds(cmds, ow_i, locks_rv));
	}
}

bool nvmeibc_disk_io_command_is_timed_out(const struct nvmeibc_disk_io_command* iocmd, ulong now)
{
	const struct operation *o = ((struct nvmeibc_block_command *)(iocmd->disk_cmd.owner))->o;
	return ((!o)||((now - o->jiffies1) >= o->nd->max_retry_jiffies)||o->topo->phased_out);
}

#define need_turn_off_db(pbcomp) ((pbcomp)->code == NVMEIBC_CMD_BLKSET_INFO_WR_DR)
static int __calc_ncmds_db_turn_off(const struct nvmeibc_block_command *rldr,
							 enum e_cmds_stage cur_stage, int *ncmds)
{
	int ci = dp_cmds_get_first_cmd_of_stage(rldr, cur_stage), last_cmd = 0;
	for (; (ci < rldr->nraid_siblings)&&(rldr[ci].my_stage == cur_stage); ci++) {
		if (need_turn_off_db(dp_cmds_get_pigbck_comp_dc(rldr[ci].iocmd))) {
			(*ncmds)++;
			last_cmd = ci;
		}
	}
	return last_cmd;
}

// Counts how many do_not_send commands of a specific stage
static int __calc_n_do_not_send_for_stage(const struct nvmeibc_block_command *rldr,
										  enum e_cmds_stage cur_stage, int *ncmds)
{
	int ci = dp_cmds_get_first_cmd_of_stage(rldr, cur_stage);
	for (; (ci < rldr->nraid_siblings)&&(rldr[ci].my_stage == cur_stage); ci++) {
		if (rldr[ci].do_not_send)
			(*ncmds)++;
	}
	return ci-1;
}

/* [first..last] are relative to raid leader, not relative to all cmds! */
static int __get_last_send_cmd(const struct nvmeibc_block_command *rldr, int first, int last,
							int *ncmds, int *non_exec, enum e_cmds_stage cur_stage)
{
	int ci = __calc_n_do_not_send_for_stage(rldr, cur_stage, non_exec);
	BUG_ON(ci != last);
	for (ci = last; ci >= first; ci--) {
		if (!rldr[ci].do_not_send) { // The last real command
			const int delta = last - ci; // Number of invalid last commands
			*non_exec-= delta;
			*ncmds   -= delta;
			return ci;
		}
	}
	return last;					// All commands in current stage are not sent
}

/* Calculate the amount of commands and last command in the current stage of
   raid leader. All calculations are relative to the raid leader */
static int __calc_ncmds_in_cur_stage(struct nvmeibc_block_command *rldr,
		int prev_stage_rv, int *ncmds /* assumes *ncmd== 0 */, int *non_exec)
{
	int last_cmd = 0;
	const enum e_cmds_stage cur_stage = rldr->raid_cur_stage;
	const bool is_write_op = (nvmeib_block_io_op_is_write(rldr->o->op));

	if (unlikely(cur_stage == E_CMDS_STAGE_CALC_DEG_DATA)) {
		// Count all the missing data segs for DO_IO_AND this will atomically inc the number of "cmds"
		// After this the stage is set, calling reed_solo function of restore data
		// This function will set the RV and call completion of number of "cmds" -> filling the BIO with missing data
		extern void __prepare_mssa_target_map_for_data_restore(const struct nvmeibc_block_command *);
		const enum e_cmds_stage read_stage = E_CMDS_STAGE_READ_PRE_DATA;
		__prepare_mssa_target_map_for_data_restore(rldr);
		last_cmd = __calc_n_do_not_send_for_stage(rldr, read_stage, ncmds);
	} else if (unlikely(cur_stage == E_CMDS_STAGE_POST_JR_RDMA)) {
		if (!rldr->use_jr_apend_stage_data_lock) {
			// Do not execute appendix and skip to next stage
		} else if (prev_stage_rv != 0) {
			*ncmds = 1;		// Journal write failed, autofail binfo write to data lock. This is crucial because the fact that previous stage failed does not mean that any specific cmd failed. Maybe topology was phased out or any other decision to stop the execution
		} else {
			*ncmds = 1;		// Binfo write is done on owner lock linked to rldr only
		}
	} else if (unlikely(cur_stage == E_CMDS_STAGE_POST_IO_RDMA)) {
		if (!rldr->use_io_apend_stages) {
			// Do not execute appendix and skip to next stage
		} else if (is_write_op) {
			if (rldr->use_io_apend_stages_data_lock) {
				if (unlikely(prev_stage_rv != 0)) {
					*ncmds = 1;		// IO write failed, autofail apendix (binfo write to data lock / turn dbits off). This is crucial because the fact that previous stage failed does not mean that any specific cmd failed. Maybe topology was phased out or any other decision to stop the execution
				} else {
					*ncmds = 1;		// Send block info to owner lock
				}
			} else {
				const enum e_cmds_stage prev_stg = cur_stage-1; // Count the commands of "real" previous stage
				if (unlikely(prev_stage_rv != 0)) {
									// Propagate the error to dbits turn off, even though it is not mandatory
				}
				last_cmd = __calc_ncmds_db_turn_off(rldr, prev_stg, ncmds);
			}
		} else {             // == NVMEIB_BLOCK_IO_OP_READ
			*ncmds = 1; 	 // View read lock only on the raid leader (last_cmd==0)
		}
	} else if (cur_stage == E_CMDS_STAGE_CALC_PARITIES) {
		extern int __calc_ncmds_calc_parity(const struct nvmeibc_block_command *, int *);
		last_cmd = __calc_ncmds_calc_parity(rldr, ncmds);
	} else {				 /* Regular non apendix stages*/
		const int first = dp_cmds_get_first_cmd_of_stage(rldr, cur_stage);
		if (first != -1) {
			*ncmds =  dp_cmds_get_stage_count(&rldr[first], rldr->nraid_siblings-first, rldr[first].my_stage);
			last_cmd = (first + *ncmds - 1);
			last_cmd = __get_last_send_cmd(rldr, first, last_cmd, ncmds, non_exec, cur_stage);
		}
	}
	return last_cmd;
}

/************************ Multi-stage execution plan **************************/
/* Finds out the previous stage execution status (rv of stage cmds).
   Note: This does not include post stage cb().
   Called before we launch the next stage to prevent further corruption of the
   slice by combination of failed stages */
int dp_cmds_prev_stage_analyze_rv(struct nvmeibc_block_command *cmds, int li)
{   /* Daniel: is this correct that at least 1 faild cmd is enough? */
	int ci, rv = 0;
	struct nvmeibc_block_command *rldr = &cmds[li];	// raid leader
	const int start = li, end = li + rldr->nraid_siblings;
	enum e_cmds_stage stg = rldr->raid_cur_stage;

	BUG_ON(rldr->my_leader != li); // Verify that I am leader.
	if (unlikely(e_cmds_stage_is_rdma_appendix(stg))|| unlikely(e_cmds_stage_is_reed_solomon(stg))){
		rv = rv_storage_of_binfo_write(rldr);
	} else {
		WARN(stg == E_CMDS_STAGE_CALC_DEG_DATA, "nvmeibc bug, no stage after this one\n");
		for (ci = start; (ci < end)&&(cmds[ci].my_stage <  stg)       ; ci++);
		for (          ; (ci < end)&&(cmds[ci].my_stage == stg)&&(!rv); ci++){
			if (cmds[ci].do_not_send && cmds[ci].o_rv == -ENXIO) { // Reset comp code
				__cmd_set_comp_err(&cmds[ci], 0);
			} else rv = cmds[ci].o_rv;
		}
	}
	return rv;
}

/* Prepares the commands for next execution stage. Input: raid leader cmd.
   1. Return cmds in the to-do stage (ncmds is 0 when all stages are completed)
   2. Updates the rv of previous stage according exec_func_on_stage_end() */
void dp_cmds_prepare_next_stage(struct nvmeibc_block_command *cmds, int li, int *last, int *ncmds, int *prev_stage_rv)
{
	int n_cmds_in_cur_stage, last_cmd_in_cur_stage = 0, n_non_exec_cmds = 0;
	struct nvmeibc_block_command *rldr = &cmds[li];	// raid leader
	struct nvmeibc_profiler *prof = (rldr->ds) ? nvmeibc_get_raid_good_path_profile_for_rwt_op(rldr->ds, rldr->o->op) : NULL;
	__ndump_operation(prepare_next_stage, cmds->o);
	nflog(t_02_sadbto, "cmds=@CMDS[@LI].stage=@STAGE, going++", cmds, li, rldr->raid_cur_stage);
	BUG_ON(rldr->my_leader != li); // Verify that I am leader.
	do { /* Execute func at stage end and search for next stage */
		if (unlikely(rldr->o->topo->phased_out)) {
			*prev_stage_rv = (*prev_stage_rv == 0) ? -EAGAIN : *prev_stage_rv;
			OPERATION_DBG_CNTR_INC(rldr->o, n_topo_phased_out);
		}

		nvmeibc_profiling_end_take_stats_for_stage(prof, rldr->o, rldr->raid_cur_stage, *prev_stage_rv);
		cmds->o->nd->dp.exec_func_on_stage_end(rldr, prev_stage_rv);
		n_non_exec_cmds = n_cmds_in_cur_stage = 0; /* Calc how many cmds are in this stage */
		if (unlikely(rldr->raid_cur_stage > rldr->raid_last_stage)) {
			goto _out; // Zero commands in the last stage. Finish operation
		}
		nvmeibc_profiling_start_take_stats_for_stage(prof, rldr->o, rldr->raid_cur_stage);

		last_cmd_in_cur_stage = __calc_ncmds_in_cur_stage(rldr, *prev_stage_rv, &n_cmds_in_cur_stage, &n_non_exec_cmds);
		last_cmd_in_cur_stage += li; // Convert relative to absolute index
		if (n_non_exec_cmds) {
			n_cmds_in_cur_stage -= n_non_exec_cmds;
		}
	} while (n_cmds_in_cur_stage == 0); // Current stage is empty, go to next

	nvmeibc_atomic_set(&rldr->n_uncompleted_cmds, n_cmds_in_cur_stage);
_out:
	*last = last_cmd_in_cur_stage;
	*ncmds= n_cmds_in_cur_stage + n_non_exec_cmds;
}

void nvmeibc_blkset_info_write_pet_describe(struct operation* o, u8 sgmnt, u64 addr, struct nvmeibc_d_rdma_comp *dc)
{
	NVMEIBC_IO_PET_MSG_NORM(&o->journal,
						"rdma.request(sgmnt=%hhu, address=0x%llx, opr=BLKSET_INFO_WRITE, binfo=0x%x<union nvmeib_blkset_info>)",
						sgmnt,
						addr,
						(u32)dc->lock.bi);
}

static void __nvmeibc_blkset_info_write_failed_to_send_pet_describe(struct operation* o, int prev_rv)
{
	NVMEIBC_IO_PET_MSG_NORM(&o->journal, "rdma.failed_to_send(prev_rv=%d)", prev_rv);
}

static void __send_all_db_turn_off(struct nvmeibc_block_command *cmds, int li,
							int n_cmds, int last_cmd, int prev_rv)
{
	int ci, err;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	/* Daniel: Loop only over commands in our stage, else when last
	   cmd is send entire operation completes and 'for' loop breaks */
	for (ci = (last_cmd + 1 - n_cmds); ci <= last_cmd; ci++) {
		struct nvmeibc_block_command *c = &cmds[ci];
		struct nvmeibc_disk_io_command *iocmd = c->iocmd;
		struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(iocmd);
		if (!need_turn_off_db(dc))
			continue;
		if (dp_cmds_pigbck_has_any(iocmd)) { /* This dirty-bit action was piggibacked */
			_NE_to_user(t_01_sadbto, DMESG_PREFIX("@DEV_NAME"), "Unexpected internal error, crashing the operating system to prevent data corruption, contact Excelero support. Error code: 1020.", cmds->o->nd->name);
			BUG();
		}
		if (prev_rv == 0) {
			nvmeibc_blkset_info_write_pet_describe(cmds->o, nvmeibc_dp_get_sgmnt_idx_from_ds(c->ds), iocmd->lpb.addr, dc);
			err = icore_ops->write_blkset_info(icore_ops, c->ds->disk, iocmd->lpb.handle, iocmd->lpb.addr, dc);
			if (err) {
				OPERATION_DBG_CNTR_INC(cmds->o, n_write_binfo_failed);

			}
		} else {
			__nvmeibc_blkset_info_write_failed_to_send_pet_describe(cmds->o, prev_rv);
			err = prev_rv;
		}
		if (err != 0) { // Implicit call to __post_cmd_dirtybit_turnoff_cb()
			/* Dont do: c->o_rv = err; Nor use rv_storage_of_binfo_write(). Daniel: Optimization: Even if dbit turn off failed it does not affect overall bio error */
			dp_cmds_complete_cmd(cmds, li, c);
		}
		/* Warning: Here 'cmds' might already be free */
	}
}

static int __send_blkset_info_to_data_lock_cb(struct nvmeibc_d_rdma_comp* dc, struct nvmeibc_d_rdma_comp_tag tag)
{
	struct nvmeibc_cmd_lock *l = lock_of_bcomp(dc), *locksets = dp_locks_get_locks_header(l);
	const int lsi = l->lockset_idx;
	int rv = 0;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();

	(void)tag;
	if (NCL_had_acquire_callback(dc->lock_status)) {
		icore_ops->cb_called_comp(icore_ops, l->ds->disk, dc);
	}
	if (unlikely(!NCL_do_i_have_owner_lock(dc->lock_status))) {
		rv = -EIO; // binfo was corrupted by non-ACID error
	}	// Else: binfo was sucessfully written

	/* Revert lock fields back, as they were before binfo_send used 'dc' */
	//dc->callback = NULL;					// Not mandatory will be set as relevant
	dc->lock_status = NCL_STATUS_TAKEN;		// Todo: Unify code with __restore_lock_status_after_rdma_op()
	dc->opr = NVMEIBC_LOCK_CMP_AND_SWAP;	// Not mandatory, this is a transport layer field. Just for easier debugability
	if (1) {    /* Give completion on cmd, Todo: Move to separate func() */
		struct nvmeibc_block_command *rldr = /* Daniel: Todo save rldr to avoid this search */ nvmeibc_cllink_find_cmd_by_lock(locksets, lsi);
		WARN(!e_cmds_stage_is_rdma_appendix(rldr->raid_cur_stage), "stage=%d\n", rldr->raid_cur_stage);
		nvmeibc_cmd_lock_response_io_pet_describe(rldr->o, l);
		if (rv_storage_of_binfo_write(rldr)) {
			WARN((rv == 0), "nvmeibc bug rldr(my=%d, cur=%d, rv=%d)\n", rldr->my_stage, rldr->raid_cur_stage, rv_storage_of_binfo_write(rldr));
		} else if (rv) {
			rv_storage_of_binfo_write(rldr) = rv;	// Here is a race, a few callbacks may be putting their 'rv' into the same integer, however never will '0' overwrite an error
			OPERATION_DBG_CNTR_INC(rldr->o, n_write_binfo_failed);
		}
		//this is hack; the rldr or any other command are not in rdma_appendix_stage; we use the function below to continue running the state machine
		//thus the response from set blockset info will not be printed
		#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR == 1
			BUG_ON(-1 != dp_cmds_get_first_cmd_of_stage(rldr, rldr->raid_cur_stage));
		#endif
		dp_cmds_complete_cmd(rldr->cmdarr, rldr->my_leader, NULL);
	}
	return 0;
}

// In absence of IO command to D0 we still need to update the binfo
static void __send_blkset_info_to_data_lock(struct nvmeibc_block_command *cmds, int li, int prev_rv)
{
	struct nvmeibc_block_command *rldr = &cmds[li];
	struct nvmeibc_cmd_lock *dl = __get_data_lock(rldr);	// dl != NULL or else ???
	struct nvmeibc_d_rdma_comp *dc = &dl->comp;
	int rv;
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	WARN(((dc->lock_status != NCL_STATUS_TAKEN) && (!((dc->lock_status == NCL_STATUS_CONTENDED || dc->lock_status == NCL_STATUS_DISKDEAD) && prev_rv))) || (dc->opr != NVMEIBC_LOCK_CMP_AND_SWAP), "status=%d, opr=%d, prev_rv=%d\n", dc->lock_status, dc->opr, prev_rv);	// Daniel: if Transferred lock - treat as taken
	WARN(!e_cmds_stage_is_rdma_appendix(rldr->raid_cur_stage), "stage=%d\n", rldr->raid_cur_stage);
	dc->lock_status = NCL_STATUS_NOTISSUED;			// Lock is taken but we use its comp for binfo
	dc->callback = __send_blkset_info_to_data_lock_cb;		// Safe to change callback, when it is used by unlock - it will be overriden

	if (prev_rv == 0) {
		/* Send the lock info */
		dc->lock.bi = nvmeibc_rldr_get_post_stage_rdma_piggyback(&rldr->rld, rldr->raid_cur_stage).all;
		nvmeibc_blkset_info_write_pet_describe(rldr->o, nvmeibc_dp_get_sgmnt_idx_from_ds(dl->ds), dl->address, dc);
		rv = icore_ops->write_blkset_info(icore_ops, dl->ds->disk, handle_of(dl->ds), dl->address, dc);
	} else {
		rv = prev_rv;
	}
	if (unlikely(rv)) {
		_ND(t_00_binfo_datalock, "Failed binfo-write data_lock=@DATA_LOCK inline cb(), rv=@RV, prev_rv=@RV", dl, rv, prev_rv);
		dc->lock_status = NCL_STATUS_DISKDEAD;
		dc->callback(dc, nvmeibc_d_rdma_comp_tag_make());
	}
}

static void __send_cmds_of_stage(struct nvmeibc_block_command *cmds, int n_cmds, int last_cmd, int prev_rv)
{
	int ci, rv, first_cmd = (last_cmd + 1 - n_cmds);
	const int error_on_no_execution = (prev_rv ? prev_rv : -ENXIO);	// Propagate previous error
	for (ci = first_cmd; ci <= last_cmd; ci++) {		// Loop only over commands in our stage
		if (unlikely(cmds[ci].do_not_send))
			continue;
		if (prev_rv)     /* Prev stage (locks/cmds) broken. Fail this stage */
			cmds[ci].iocmd->comp.comp_code = prev_rv;
		rv = dp_cmds_tryexec_cmd(cmds, ci, error_on_no_execution);
		if ((rv < 0) && (!prev_rv)) /* Also abort the rest of stage cmds */
			prev_rv = error_on_no_execution;		// Propagate previous error
	} /* Warning: Here cmds can be already free() */
}

static void __handle_stage_post_io_rdma(struct nvmeibc_block_command *cmds, int li, int last_cmd, int prev_rv)
{
	struct nvmeibc_disk_io_command *iocmd = cmds[last_cmd].iocmd;
	struct nvmeibc_cmd_lock *l = iocmd->comp.pigbck_lock;
	WARN(!dp_cmd_is_raid_leader(&cmds[last_cmd]), "nvmeibc bug ci=%d\n", last_cmd);
	if (!prev_rv) {
		struct nvmeibc_d_rdma_comp *dc = dp_cmds_get_pigbck_comp_dc(iocmd);
		dp_locks_view_lock_sm(dc, nvmeibc_d_rdma_comp_tag_make());  // == dc->callback(dc);
	} else {				// Read command failed, skip viewing lock
		dp_locks_read_complete(dp_locks_get_locks_header(l), l->lockset_idx, NCL_STATUS_DONE);
		dp_cmds_complete_cmd(cmds, li, NULL);
	}
}

static void __exec_stage(struct nvmeibc_block_command *cmds, int li, int prev_rv, int n_cmds, int last_cmd)
{
	const enum e_cmds_stage cur_stage = cmds[li].raid_cur_stage;
	BUG_ON(n_cmds == 0);	// IO will get stuck!
	if (unlikely(e_cmds_stage_is_rdma_appendix(cur_stage))) {
		if (cur_stage == E_CMDS_STAGE_POST_JR_RDMA) {
				__send_blkset_info_to_data_lock(cmds, li, prev_rv);
		} else if (cur_stage != E_CMDS_STAGE_POST_IO_RDMA) {
			WARN(true, "nvmeibc bug: wrong appendix stage=%d\n", cur_stage);
		} else if (cmds[li].use_io_apend_stages_data_lock) {
			__send_blkset_info_to_data_lock(cmds, li, prev_rv);
		} else if (nvmeib_block_io_op_is_write(cmds[last_cmd].iocmd->reqs1.op)) {
			__send_all_db_turn_off(cmds, li, n_cmds, last_cmd, prev_rv);
		} else {
			__handle_stage_post_io_rdma(cmds, li, last_cmd, prev_rv);
		}
	} else if (unlikely(cur_stage == E_CMDS_STAGE_CALC_DEG_DATA)) {  /*Degraded reads*/
		extern void restore_degraded_data_for_read(const struct operation *o, u32 dgrd_segment_bmp, int prev_rv, const bool is_crc_required);
		struct operation *o = cmds[li].o;
		/* We have all required blocks, calc data and complete commands */
		restore_degraded_data_for_read(o, nvmeibc_raid1_get_inverse_roles_bmp(nvmeibc_disk_segment_get_praid(o->cmds->ds), o->mssa->owner_seg, readable), prev_rv, false);
	} else if (unlikely(cur_stage == E_CMDS_STAGE_CALC_PARITIES)) {  /* Parity calculation */
		extern u32 apply_gf_calculation_for_operation(const struct operation *o, u32 dgrd_sgmnts_bmp, int prev_rv);
		struct operation *o = cmds[li].o;
		apply_gf_calculation_for_operation(o, nvmeibc_raid1_get_inverse_roles_bmp(nvmeibc_disk_segment_get_praid(o->cmds->ds), o->mssa->owner_seg, readable), prev_rv);
	} else {
		const bool is_disk_op = ((cur_stage == E_CMDS_STAGE_READ_PRE_DATA)||(cur_stage == E_CMDS_STAGE_WRITE_JOURNAL)||(cur_stage == E_CMDS_STAGE_DO_IO_AND_PAR)); // LKJ, todo, remove in product, just for debug
		WARN((!is_disk_op), "nvmeibc bug: cant exec stage=%d rv=%d\n", cur_stage, prev_rv);	// Unknown stage or should have skipped it
		__send_cmds_of_stage(cmds, n_cmds, last_cmd, prev_rv);
	}
	/* Be careful: Here cmds/op/locks might already be kfree() */
}

void dp_cmds_execute_first_stage(struct nvmeibc_block_command *cmds, int li, int locks_rv)
{
	int n_cmds = 0, tmp = 0, last_cmd = li + __calc_ncmds_in_cur_stage(&cmds[li], locks_rv, &n_cmds, &tmp);
	__attribute__ ((unused)) struct operation *o = cmds->o;
	BLKCMP_IO_ASYNC_AWAIT(__exec_stage(cmds, li, locks_rv, n_cmds, last_cmd));
}

static int __prepare_and_exec_next_stage(struct nvmeibc_block_command *cmds, int li, int *prev_rv)
{
	__attribute__ ((unused)) struct operation *o = cmds->o;
	int n_cmds, last_cmd = 0;   /* in the to-execute stage */
	dp_cmds_prepare_next_stage(cmds, li, &last_cmd, &n_cmds, prev_rv);
	*prev_rv = (*prev_rv == 0) ? 0 : -ENXIO;  /* If Stage failed -> abort next stages */
	if (n_cmds) {
		BLKCMP_IO_ASYNC_AWAIT(__exec_stage(cmds, li, *prev_rv, n_cmds, last_cmd));
		/* Be careful: Here cmds/op/locks might already be kfree() */
	}
	return n_cmds;
}

void dp_cmds_next_stage_execute(struct nvmeibc_block_command *cmds, int ci)
{	// Handle next stage if required
	struct nvmeibc_block_command *rldr = &cmds[ci];
	__attribute__ ((unused)) struct operation *o = cmds->o;
	int rv = dp_cmds_prev_stage_analyze_rv(cmds, ci);
	const int stage_rv = rv;
	if (rldr->use_stages) {
		if ((rldr->raid_cur_stage < rldr->raid_last_stage) && __prepare_and_exec_next_stage(cmds, ci, &rv))
			return; /* Be careful: Here cmds/op/locks might already be kfree() */
	}
	if (rldr->raid_cur_stage <= rldr->raid_last_stage)
		nvmeibc_profiling_end_take_stats_for_stage(nvmeibc_get_raid_good_path_profile_for_rwt_op(cmds->ds, rldr->o->op), cmds->o, rldr->raid_cur_stage, stage_rv);	// For last stage
	nflog(t_2ctns, "@FUNCTION: o=@OPERATION.uncomp_raid=@UNCOMP_RAID, by cmds=@CMDS[@CI].sibs=@SIBS", __FUNCTION__, cmds->o, nvmeibc_atomic_read(&cmds->o->n_uncomp_raids), cmds, ci, rldr->nraid_siblings);
	if (rldr->use_stages) {
		rldr->all_cmds_sm_done = true;
		BLKCMP_IO_ASYNC_AWAIT(__release_locks_of_completed_command(cmds, ci));
	}
	BLKCMP_IO_ASYNC_RESUME_CAL(nvmeibc_operation_put(cmds->o, rldr->nraid_siblings));
}

#ifdef BLKCMP_IO_COMPLETION_PRESERVE_STACK
void dp_cmds_fiber_execute_1_blockset_state_machine(struct operation *o, const bool acquired_locks)
{
	struct nvmeibc_block_command *cmds = o->cmds;
	if (acquired_locks) {
		sgmnts_bmp_t bmp = dp_locks_stale_get_locks_bitmap_in_blockset(o->locks);
		while (bmp) {
			const int sync_rv = dp_locks_stale_call_sync_blockset(o->locks, bmp);
			BLKCMP_IO_ASYNC_AWAIT(dp_locks_stale_after_sync_retry_acquire(o->locks, sync_rv, bmp));
			bmp = dp_locks_stale_get_locks_bitmap_in_blockset(o->locks);
		}
		dp_transition_to_locked_cmds_sm(o->locks, 0);
	}

	while (!cmds->all_cmds_sm_done) {
		BLKCMP_IO_ASYNC_AWAIT(dp_cmds_done_stage_overcome_failure(cmds, 0));
		if (cmds->use_read_fail_fix_blockset)
			BLKCMP_IO_ASYNC_AWAIT(dp_cmds_done_stage_overcome_failure(cmds, 0));
		dp_cmds_next_stage_execute(cmds, 0);
		while (cmds->should_check_view_lock) {	// May need to retry it due to lock taken / sync-stale / etc ...
			cmds->should_check_view_lock = 0;
			BLKCMP_IO_ASYNC_AWAIT(dp_locks_view_lock_sm(dp_cmds_get_pigbck_comp_dc(cmds->iocmd), nvmeibc_d_rdma_comp_tag_make()));
		}
	};
	if (acquired_locks) {
		nvmeibc_atomic_sub(LARGE_DEBUG_VALUE, &o->locks->n_uncompleted_locks);
		BLKCMP_IO_ASYNC_AWAIT(dp_transition_to_unlocked_blockset_sm(cmds, 0, 0));
	}
	nvmeibc_operation_put(cmds->o, cmds->nraid_siblings);	// Here operation will get free, and locks will get free
}
#endif

/********************** API of raid leaders commands **************************/
void dp_rldr_set_wr_journal_ndb_from_data(struct nvmeibc_block_command *rldr, const int n_jcmds, const int journal_start)
{
	int j;
	#if !NVMESH_IS_PRODUCTION_COMPILATION
		extern bool qa_ec_stress_debug;	// Refactor
	#endif
	BUG_ON(rldr->o->mssa->no_jour);
	for (j = journal_start; j < journal_start + n_jcmds; j++) {
		rldr[j].iocmd->reqs1.ndb = rldr[j + n_jcmds].iocmd->reqs1.ndb;
		rldr[j].is_not_ndb_owner = true;
		if (unlikely((rldr[j].do_not_send) || !nvmeibc_ec_reuse_req
			#if !NVMESH_IS_PRODUCTION_COMPILATION
		 		|| qa_ec_stress_debug
			#endif
		)) {
			/* dont save cookie, force client to resend data increasing chance of roll-fwd */
		} else {
			get_rcookie_ptr(rldr[j].iocmd)->action = nvmeib_data_reuse_buf_SAVE;
		}
	}
}

void dp_rldr_set_wr_journal_cookies_to_data(struct nvmeibc_block_command *rldr)
{
	const int n_jcmds = rldr->o->mssa->n_writes / 2;
	const int jc = 		rldr->o->mssa->n_reads;
	int j;
	BUG_ON(rldr->o->mssa->no_jour);
	for (j = jc; j < jc + n_jcmds; j++) {
		struct nvmeib_data_reuse_buf_params *rcookie = get_rcookie_ptr(rldr[j].iocmd);
		if (rcookie->action) { /* If cookie exists, can reuse it */
			if (rcookie->channel_ver)	// Ask to send only md, reuse data
				rcookie->action = nvmeib_data_reuse_buf_SEND_REL;
			else
				nvmeib_data_reuse_buf_zero(rcookie);	// Save failed, dont ask anything
		}
	}
}

struct nvmeibc_block_command *dp_cmd_jour_to_data(struct nvmeibc_block_command *c)
{
	struct nvmeibc_block_command *rldr = dp_cmd_get_raid_leader(c);
	BUG_ON(c->my_stage == E_CMDS_STAGE_WRITE_JOURNAL && rldr->o->mssa->no_jour);
	return (c->my_stage == E_CMDS_STAGE_WRITE_JOURNAL) ? (c + (rldr->o->mssa->n_writes / 2)) : c;
}

// Find a lock by segment in a lock array. Will return NULL if not found
struct nvmeibc_cmd_lock *__get_lock_by_seg(const struct nvmeibc_disk_segment *seg,
					   struct nvmeibc_cmd_lock *lock_start)
{
	int i;
	for (i=0; (i<lock_start->n_siblings); i++) {
		if (lock_start[i].ds == seg)
			return &lock_start[i];
	}
	return NULL;
}

struct nvmeibc_cmd_lock * __get_data_lock(struct nvmeibc_block_command *rldr)
{
	const int ssi = rldr->o->mssa->owner_seg;
	const struct nvmeibc_raid1 *pr = nvmeibc_disk_segment_get_praid(rldr->ds);
	const struct nvmeibc_disk_segment *ssds = &pr->segments[ssi];
	const int o_lsi = nvmeibc_cllink_find_lock_by_cmd(rldr);
	struct nvmeibc_cmd_lock *o_lock = &rldr->cmdarr->locksets[o_lsi];
	return __get_lock_by_seg(ssds, o_lock);
}

/**************************** API of gen commands *****************************/
int dp_cmds_gencmd_add(struct nvmeibc_block_command *cmd)
{
	const gfp_t gfp = nvmeibc_dp_get_allow_io_gfp_flags();
	if ((cmd->gen_cmd = kzalloc(sizeof(*cmd->gen_cmd), gfp)) != NULL) {
		DEBUG_TRANSFERS_init_cb_counter(cmd);
		cmd->gen_cmd->disk_cmd.owner = cmd->iocmd;
		return 0;
	}
	_NT(trace_dp_io_generic_cmds_dp_cmds_gencmd_add, "Allocation failure");
	return -ENOMEM;
}

void dp_cmds_gencmd_del(struct nvmeibc_block_command *cmd)
{
	if (cmd->gen_cmd) {
		void *save = cmd->gen_cmd->disk_cmd.owner;
		memset(cmd->gen_cmd, 0, sizeof(*cmd->gen_cmd));	// Daniel: Possibly overkill
		cmd->gen_cmd->disk_cmd.owner = save;
	}
}
