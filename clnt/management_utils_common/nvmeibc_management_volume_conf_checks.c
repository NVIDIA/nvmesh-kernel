#include "common/kr_incs.h"
#include "clnt/block/datapath_utils_generic/nvmeibc_block_dp_defs.h"
#include "common/nvmeib_shared.h"
#include "common/nvmeib_version_shared.h"
#include "common/nvmeib_volume_type.h"
#include "nvmeibc_management_volume_conf_checks.h"
#include "nvmeibc_management_capi_parse_conf.h"

int __get_valid_stripe_size(const struct nvmeibc_chunk_conf *cur_chunk, const struct nvmeibc_praid_conf *cur_praid, int chunk_idx)
{
	int stripe_size = cur_chunk->stripeSize << MGMT2CLNT_SHIFT;
	const int slice_size = cur_praid->dataBlocks; //Slice size is in the number of data segs
	if (!slice_size) {
		_NE(t3cnmtc, "Illegal slice_size=@INT ch=@CHUNK_IDX", slice_size, chunk_idx);
		return -EINVAL;
	}

	if ((stripe_size == 0) || (stripe_size % (LOCKSET_SLICES * slice_size))) {
		// Stripe vs slice missmatch!
		if (cur_chunk->stripeWidth > 1) {	// Config error!
			_NE(t1cnmtc, "Illegal stripe_size=@INT slice_size=@INT stripe_width=@INT ch=@CHUNK_IDX",
			   stripe_size, slice_size, cur_chunk->stripeWidth, chunk_idx);
			return -EINVAL;
		} else {	// Stripe size not needed anyways
			stripe_size = (LOCKSET_SLICES*slice_size);
			_NE(t2cnmtc, "stripe size=0, overriden to=@STRIPE_SIZE, ch=@CHUNK_IDX", stripe_size, chunk_idx);
			return stripe_size;
		}
	}
	return stripe_size;
}

void nvmeibc_management_try_setup_stripe_size_safe(struct nvmeibc_chunk_conf *cur_chunk, const struct nvmeibc_praid_conf *cur_praid, int chunk_idx)
{
	if (!cur_chunk){
		return;
	}
	if (!cur_praid){
		return;
	}
	{
		const int stripe_size = __get_valid_stripe_size(cur_chunk, cur_praid, chunk_idx);
		if (stripe_size <= 0){
			return;
		}
		if (stripe_size != (cur_chunk->stripeSize << MGMT2CLNT_SHIFT)){
			cur_chunk->stripeSize = (stripe_size >> MGMT2CLNT_SHIFT);
		}
	}
}

uint nvmeibc_reject_all_volume_attaches = false;
module_param_named(reject_all_volume_attaches, nvmeibc_reject_all_volume_attaches, uint, 0644);
MODULE_PARM_DESC(reject_all_volume_attaches, "Autofail all volume attaches, prevent kernel crashes on wrong MCS/mgmt messages");

int __check_striping_length_and_chunk(u32 binje, const struct nvmeibc_volume_conf *conf)
{
	const struct nvmeibc_chunk_conf *chunks = conf->chunks;
	const int num_chunks = conf->n_chunks;
	u64 exp_lba = 0;
	int c, praid_idx, segment_idx, rv = 0, chunk_count = 0;
	NFIN;
	if (binje % conf->sliceWidth) { // Check Snake size is divisible by Multisnake size (N%S) For non-QLC volumes only
		_NT(error_volume_check_snake_and_n, "Illegal binje=@BINJE is not divisible by sliceWidth=@INT", binje, conf->sliceWidth);
		rv = -EINVAL;
		goto out;
	}
	if (nvmeibc_reject_all_volume_attaches) {
		WARN(nvmeibc_reject_all_volume_attaches, "Volume %s: Froce Autofailing attach\n", &conf->name[0]);
		rv = -EINVAL;
		goto out;
	}

	for (c = 0, exp_lba = 0; c < num_chunks; c++) {
		const struct nvmeibc_chunk_conf *cur_chunk = &chunks[c];
		const u64 chunk_length = (&cur_chunk->praids[0].segments[0] != NULL) ?
			calc_segment_length(&cur_chunk->praids[0].segments[0]) : 0;


		if (cur_chunk->vlbs != exp_lba) {
			_NE(error_9_volume_check_striping_length_and_chunk,  "Chunk LBA mismatch: @CI) exp_lba=@EXP_LBA, first_vlba=@VLBA",
			   c, exp_lba, cur_chunk->vlbs);
			rv = -EINVAL;
			goto out;
		}

		for (praid_idx = 0; praid_idx < cur_chunk->n_praids; praid_idx++) {
			const struct nvmeibc_praid_conf *cur_praid = &cur_chunk->praids[praid_idx];
			const int n_max_segs = cur_praid->numberOfMirrors+cur_praid->dataBlocks+cur_praid->parityBlocks;
			if (cur_praid->stripeIndex != (unsigned int)praid_idx) { //vcfg@"validate stripes are ordered by the index"
				_NT(error_volume_check_striping_length_and_chunk, "Praid stripeIndex @STRIPEINDEX is not ordered, expected @PRAID_IDX", cur_praid->stripeIndex, praid_idx);
				rv = -EINVAL;
				goto out;
			}
			if (cur_praid->n_segments > N_MAX_RAID_SLICE_LEN) { //vcfg@"ensure the number of segments does not exceed the maximum supported"
				_NT(error_5_volume_check_striping_length_and_chunk, "Illegal n_segments=@N_SEGMENTS > N_MAX_RAID_SLICE_LEN, ch=@CHUNK_IDX praid=@PRAID_IDX", cur_praid->n_segments, c, praid_idx);
				rv = -EINVAL;
				goto out;
			}
			for (segment_idx = 0; segment_idx < cur_praid->n_segments; segment_idx++) {
				const struct nvmeibc_segment_conf *cur_seg =
												  &cur_praid->segments[segment_idx];
				if (segment_idx == 0){
					exp_lba += cur_praid->dataBlocks * calc_segment_length(cur_seg);
				}
				if (cur_seg->pRaidTypeIndex != (unsigned int)segment_idx) { //vcfg@"ensure the segments are ordered by the index"
					_NT(error_1_volume_check_striping_length_and_chunk, "Segment pRaidTypeIndex @PRAIDTYPEINDEX is not ordered, expected @SI", cur_seg->pRaidTypeIndex, segment_idx);
					rv = -EINVAL;
					goto out;
				}
				_NT(trace_volume_check_striping_length_and_chunk, "segment.diskuuid=@DISKUUID, ruuid=@SEG, stripe_index=@STRIPE_INDEX, n_segs=@N_SEGMENTS, length=@LENGTH",
				   cur_seg->diskUUID, cur_seg->uuid,
				   get_allocation_index(n_max_segs,cur_praid, cur_seg),
				   cur_praid->n_segments, calc_segment_length(cur_seg));
				if (get_allocation_index(n_max_segs, cur_praid, cur_seg) >=
					(unsigned int)(n_max_segs * cur_chunk->stripeWidth)) { //vcfg@"validate that segment index does not exceed the stripeWidth"
					_NT(error_2_volume_check_striping_length_and_chunk, "Invalid striping: disk=@DISK_NAME, r=@SEG, stripe_i=@STRIPE_I, rep=@REP", cur_seg->diskUUID, cur_seg->uuid, get_allocation_index(n_max_segs, cur_praid, cur_seg), n_max_segs);
					rv = -EINVAL;
					goto out;
				}
				if (chunk_length != calc_segment_length(cur_seg)) { //vcfg@"validate all segments in chunk has the same size"
					_NT(error_3_volume_check_striping_length_and_chunk, "Invalid segment_length=@SEGMENT_LENGTH differs from chunk length (@CHUNK_LENGTH)", calc_segment_length(cur_seg), chunk_length);
					rv = -EINVAL;
					goto out;
				}

				if (cur_seg->lbs % LOCKSET_4KS) {
					_NT(error_6_volume_check_striping_length_and_chunk, "Segment is not aligned! disk=@DISK_NAME, r=@SEG, seg_@DLBA",
						cur_seg->diskUUID, cur_seg->uuid, cur_seg->lbs);
					rv = -EINVAL;
					goto out;
				}

				if (get_allocation_index(n_max_segs, cur_praid, cur_seg) == 0) {
					chunk_count++;
				}
			}
		}
	}
	if (conf->n_chunks != chunk_count) { //vcfg@"chunks count is derived from segments indexing and from the struct variable - they have to be the same"
		rv = -EINVAL;
		_NT(error_4_volume_check_striping_length_and_chunk, "missing chunks: expected=@N_CHUNKS, actual=@RV", conf->n_chunks, rv);
		goto out;
	}
	if (conf->blocks != exp_lba) {
		rv = -EINVAL;
		_NT(error_8_volume_check_striping_length_and_chunk, "sum of all raid data segments and the volume size are different. size by raid=@VLBA by config=@VLBA, actual=@RV"
				, exp_lba << MGMT2CLNT_SHIFT, conf->blocks << MGMT2CLNT_SHIFT, rv);
		goto out;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibc_management_does_vol_have_disks(const struct nvmeib_mgmt_to_client_volume_configuration* cfg)
{
	int t_idx = 0;
	if (!cfg) {
		return 0;
	}

	for(t_idx = 0; t_idx < cfg->n_targets; ++t_idx) {
		const struct nvmeibc_target_conf* target = &cfg->targets[t_idx];
		if (target->n_disks) {
			return 1;
		}
	}
	return 0;
}

int nvmeibc_management_um_checks(const struct nvmeibc_volume_conf* vol_conf)
{
	int rv = 0;
	int i = 0;
	int r = 0;
	NFIN;
	if (!vol_conf){
		_NT(error_nvmeibc_management_um_checks_1, "Invalid conf, volume is null; dummy=@INT", 0);
		rv = -EINVAL;
		goto out;
	}
	/* validate */
	if (vol_conf->n_chunks < 1) {
		_NT(error_nvmeibc_management_um_checks_2, "Invalid conf, n_chunks=@INT", vol_conf->n_chunks);
		rv = -EINVAL;
		goto out;
	}
	if (vol_conf->chunks[0].vlbs != 0) {
		_NT(error_nvmeibc_management_um_checks_3, "Invalid conf, chunks[0].vlbs=@LLU", vol_conf->chunks[0].vlbs);
		rv = -EINVAL;
		goto out;
	}
	for (i = 0; i < vol_conf->n_chunks; i++) {
		struct nvmeibc_chunk_conf* chunk = &vol_conf->chunks[i];
		for(r=0; r < chunk->n_praids; ++r){
			struct nvmeibc_praid_conf* praid = &chunk->praids[r];
			const int stripe_size = __get_valid_stripe_size(chunk, praid, i);
			if (stripe_size <= 0){
				rv = -EINVAL;
				goto out;
			}
		}
		if (i) {
			if (vol_conf->chunks[i].vlbs != vol_conf->chunks[i - 1].vlbe + 1) {
				_NT(error_nvmeibc_management_um_checks_4, "Invalid conf, chunks[@INT].vlbs=@LLU vs. chunks[@INT].vlb=@LLU",
					 i, vol_conf->chunks[i].vlbs, i -1, vol_conf->chunks[i - 1].vlbe);
				rv = -EINVAL;
				goto out;
			}
		}
	}
	if (vol_conf->chunks[vol_conf->n_chunks - 1].vlbe + 1 != vol_conf->blocks) {
		_NT(error_nvmeibc_management_um_checks_5, "Invalid conf, last chunk vlbe=@LLU vs. blocks=@LLU",
			 vol_conf->chunks[vol_conf->n_chunks - 1].vlbe, vol_conf->blocks);
		rv = -EINVAL;
		goto out;
	}
out:
	NFOUT;
	return rv;
}

void nvmeibc_management_init_defaults_safe(struct nvmeibc_volume_conf* vol_conf)
{
	int i = 0;
	struct nvmeibc_chunk_conf* chunk = NULL;

	if (!vol_conf){
		return;
	}
	if (!vol_conf->n_chunks || !vol_conf->chunks){
		return;
	}
	for (i = 0; i < vol_conf->n_chunks; i++) {
		chunk = &vol_conf->chunks[i];

		if (!chunk){
			continue;
		}

		_NT(error_nvmeibc_management_init_defaults_safe_1, "chunk[@INT]: stripe-width=@INT, stripe-size=@INT, n-praids=@INT",
			  i, chunk->stripeWidth, chunk->stripeSize, chunk->n_praids);

		if (chunk->stripeWidth == 0) {
			_NT(error_nvmeibc_management_init_defaults_safe_2, "override chunk[@INT]'s stripe-width (0) with volume's (@INT)",
				  i, vol_conf->stripeWidth);
			chunk->stripeWidth= vol_conf->stripeWidth;
		}

		if (chunk->stripeSize == -1) {
			_NT(error_nvmeibc_management_init_defaults_safe_3, "override chunk[@INT]'s stripe-size (-1) with volume's (@INT)",
				  i, vol_conf->stripeSize);
			chunk->stripeSize = vol_conf->stripeSize;
		}

		if (chunk->stripeWidth != chunk->n_praids) {
			_NT(error_nvmeibc_management_init_defaults_safe_4, "HACK: override chunk[@INT]'s stripe-width (@INT) with its n_praids (@INT)\n",
				   i, chunk->stripeWidth, chunk->n_praids);
			chunk->stripeWidth = chunk->n_praids;
		}
		if (chunk->n_praids && chunk->praids){
			nvmeibc_management_try_setup_stripe_size_safe(chunk, &chunk->praids[0], i);
		}
	}
}

